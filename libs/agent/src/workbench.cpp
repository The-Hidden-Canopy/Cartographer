#include <carto/agent/workbench.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace carto::agent {
namespace {

constexpr std::size_t kMaxGoalBytes = 1024U;
constexpr std::size_t kMaxStepIntentBytes = 1024U;
constexpr std::size_t kMaxStepIdBytes = 64U;
constexpr std::size_t kMaxTaskIdBytes = 64U;
constexpr std::size_t kMaxScopeObjects = 512U;
constexpr std::size_t kMaxPlanSteps = 256U;
constexpr std::size_t kMaxRepairAttempts = 4U;
constexpr std::uint32_t kKnownEffects = ::carto::agent::effect_mask(Effect::geometry) |
    ::carto::agent::effect_mask(Effect::topology) |
    ::carto::agent::effect_mask(Effect::transform);

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

std::uint32_t clarification_impact_weight(ClarificationImpact impact) noexcept {
    switch (impact) {
    case ClarificationImpact::object_scope: return 25U;
    case ClarificationImpact::topology_deletion: return 20U;
    case ClarificationImpact::familiarity_critical_feature: return 25U;
    case ClarificationImpact::collision_or_navigation: return 15U;
    case ClarificationImpact::interaction_anchor: return 12U;
    case ClarificationImpact::source_hierarchy: return 15U;
    case ClarificationImpact::large_spatial_transform: return 10U;
    case ClarificationImpact::imported_source_integrity: return 20U;
    case ClarificationImpact::publication_readiness: return 8U;
    }
    return 0U;
}

void append_field(std::string& output, std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

void append_number(std::string& output, std::uint64_t value) {
    append_field(output, std::to_string(value));
}

void append_double(std::string& output, double value) {
    char buffer[64]{};
    const auto converted = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (converted.ec == std::errc{}) {
        append_field(output, std::string_view(buffer,
            static_cast<std::size_t>(converted.ptr - buffer)));
    }
}

void append_object_ids(std::string& output, std::vector<scene::ObjectId> ids) {
    std::sort(ids.begin(), ids.end());
    append_number(output, ids.size());
    for (const auto id : ids) append_number(output, id.value);
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

assets::Sha256Digest clarification_context_digest(
    const AuthoringObjective& objective) {
    std::string canonical = "cartographer.clarification-context.v1|";
    append_field(canonical, objective.id);
    append_field(canonical, objective.goal);
    if (!objective.recipe_id.empty()) {
        append_field(canonical, "recipe");
        append_field(canonical, objective.recipe_id);
    }
    append_number(canonical, objective.base_revision.value());
    append_object_ids(canonical, objective.scope_objects);
    append_object_ids(canonical, objective.protected_objects);
    auto constraints = objective.hard_constraints;
    std::sort(constraints.begin(), constraints.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    append_number(canonical, constraints.size());
    for (const auto& constraint : constraints) {
        append_field(canonical, constraint.id);
        append_number(canonical, static_cast<std::uint64_t>(constraint.kind));
        append_number(canonical, constraint.object.has_value() ? 1U : 0U);
        if (constraint.object.has_value()) append_number(canonical, constraint.object->value);
        append_double(canonical, constraint.maximum_displacement);
        append_field(canonical, constraint.rationale);
    }
    auto questions = objective.clarifications;
    std::sort(questions.begin(), questions.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    append_number(canonical, questions.size());
    for (const auto& context_question : questions) {
        append_field(canonical, context_question.id);
        append_field(canonical, context_question.prompt);
        append_field(canonical, context_question.rationale);
        append_number(canonical, context_question.requires_explicit_answer ? 1U : 0U);
        auto impacts = context_question.impacts;
        std::sort(impacts.begin(), impacts.end());
        append_number(canonical, impacts.size());
        for (const auto impact : impacts) {
            append_number(canonical, static_cast<std::uint64_t>(impact));
        }
        append_object_ids(canonical, context_question.affected_objects);
        auto constraint_ids = context_question.affected_constraint_ids;
        std::sort(constraint_ids.begin(), constraint_ids.end());
        append_number(canonical, constraint_ids.size());
        for (const auto& id : constraint_ids) append_field(canonical, id);
        append_number(canonical, context_question.downstream_plan_branches);
        auto candidates = context_question.candidates;
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        append_number(canonical, candidates.size());
        for (const auto& candidate : candidates) {
            append_field(canonical, candidate.id);
            append_field(canonical, candidate.label);
            append_object_ids(canonical, candidate.affected_objects);
        }
    }
    return digest_text(canonical);
}

assets::Sha256Digest clarification_answer_digest(const ClarificationQuestion& question) {
    std::string canonical = "cartographer.clarification-answer.v1|";
    append_field(canonical, question.id);
    append_number(canonical, question.selected_candidate_id.has_value() ? 1U : 0U);
    if (question.selected_candidate_id.has_value()) {
        append_field(canonical, *question.selected_candidate_id);
    }
    append_number(canonical, question.freeform_answer.has_value() ? 1U : 0U);
    if (question.freeform_answer.has_value()) append_field(canonical, *question.freeform_answer);
    return digest_text(canonical);
}

assets::Sha256Digest clarification_plan_context_digest(const AgentPlan& plan) {
    std::string canonical = "cartographer.clarification-plan-context.v1|";
    append_field(canonical, plan.id);
    append_field(canonical, clarification_context_digest(plan.objective).hex());
    append_number(canonical, plan.steps.size());
    for (const auto& step : plan.steps) {
        append_field(canonical, step.id);
        append_number(canonical, step.target.object.value);
        append_number(canonical, step.target.component.index());
        if (const auto* face = std::get_if<geometry::FaceId>(&step.target.component)) {
            append_number(canonical, face->value);
        } else if (const auto* vertex = std::get_if<geometry::VertexId>(&step.target.component)) {
            append_number(canonical, vertex->value);
        }
        append_field(canonical, step.intent);
        auto dependencies = step.dependencies;
        std::sort(dependencies.begin(), dependencies.end());
        append_number(canonical, dependencies.size());
        for (const auto& dependency : dependencies) append_field(canonical, dependency);
    }
    return digest_text(canonical);
}

core::Vec3d transform_point(const core::Transform& transform, core::Vec3d point) noexcept {
    return transform.translation + transform.rotation.rotate(
        core::componentwise_multiply(transform.scale, point));
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

core::Result<AgentDemonstrationParameters> parameterize_demo_step(
    const ai::Proposal& proposal,
    const project::ProjectDocument& document,
    const TargetRef& target) {
    AgentDemonstrationParameters parameters;
    if (proposal.preview_kind == editor::PreviewKind::inset_face ||
        proposal.preview_kind == editor::PreviewKind::extrude_face) {
        const auto* face_id = std::get_if<geometry::FaceId>(&target.component);
        const auto* object = document.scene().find(target.object);
        if (face_id == nullptr || object == nullptr || !object->mesh_asset.has_value() ||
            !proposal.parameters.distance.has_value()) {
            return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
                "face demonstration parameters could not resolve the validated target"));
        }
        const auto mesh = document.meshes().find(*object->mesh_asset);
        if (mesh == document.meshes().end()) {
            return core::Result<AgentDemonstrationParameters>::failure(stale(
                "face demonstration parameters could not resolve the source mesh"));
        }
        const auto* face = mesh->second.find_face(*face_id);
        if (face == nullptr || face->vertices.size() < 3U) {
            return core::Result<AgentDemonstrationParameters>::failure(stale(
                "face demonstration parameters could not resolve the source face"));
        }
        double longest_edge = 0.0;
        for (std::size_t index = 0U; index < face->vertices.size(); ++index) {
            const auto* first = mesh->second.find_vertex(face->vertices[index]);
            const auto* second = mesh->second.find_vertex(
                face->vertices[(index + 1U) % face->vertices.size()]);
            if (first == nullptr || second == nullptr) {
                return core::Result<AgentDemonstrationParameters>::failure(stale(
                    "face demonstration parameters found a missing source vertex"));
            }
            const double edge_length = (second->position - first->position).length();
            if (!std::isfinite(edge_length)) {
                return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
                    "face demonstration parameters found a non-finite source edge"));
            }
            longest_edge = std::max(longest_edge, edge_length);
        }
        if (!(longest_edge > 0.0)) {
            return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
                "face demonstration parameters require a non-zero source edge"));
        }
        parameters.parameterization =
            AgentDemonstrationParameterization::normalized_face_distance;
        parameters.normalized_distance = *proposal.parameters.distance / longest_edge;
    } else if (proposal.preview_kind == editor::PreviewKind::object_translation_delta) {
        if (proposal.tool_id != "object.translate" ||
            !std::holds_alternative<std::monostate>(target.component) ||
            !proposal.parameters.position.has_value()) {
            return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
                "translation demonstration parameters do not match the validated target"));
        }
        parameters.parameterization =
            AgentDemonstrationParameterization::relative_displacement;
        parameters.relative_displacement = *proposal.parameters.position;
    } else if (proposal.preview_kind == editor::PreviewKind::set_vertex_position) {
        const auto* vertex_id = std::get_if<geometry::VertexId>(&target.component);
        const auto* object = document.scene().find(target.object);
        if (vertex_id == nullptr || object == nullptr || !object->mesh_asset.has_value() ||
            !proposal.parameters.position.has_value()) {
            return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
                "vertex demonstration parameters could not resolve the validated target"));
        }
        const auto mesh = document.meshes().find(*object->mesh_asset);
        if (mesh == document.meshes().end()) {
            return core::Result<AgentDemonstrationParameters>::failure(stale(
                "vertex demonstration parameters could not resolve the source mesh"));
        }
        const auto* vertex = mesh->second.find_vertex(*vertex_id);
        if (vertex == nullptr) {
            return core::Result<AgentDemonstrationParameters>::failure(stale(
                "vertex demonstration parameters could not resolve the source vertex"));
        }
        parameters.parameterization =
            AgentDemonstrationParameterization::relative_displacement;
        parameters.relative_displacement = *proposal.parameters.position - vertex->position;
    } else {
        return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
            "native preview kind has no safe demonstration parameterization"));
    }
    if (auto result = parameters.validate(proposal.tool_id); !result) {
        return core::Result<AgentDemonstrationParameters>::failure(result.error());
    }
    return core::Result<AgentDemonstrationParameters>::success(std::move(parameters));
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

bool same_scene_object(
    const scene::SceneObject* left,
    const scene::SceneObject* right) noexcept {
    if (left == nullptr || right == nullptr) return left == right;
    return left->id == right->id && left->name == right->name &&
        same_transform(left->local_transform, right->local_transform) &&
        left->parent == right->parent && left->mesh_asset == right->mesh_asset &&
        left->visible == right->visible && left->locked == right->locked;
}

bool same_mesh_state(
    const geometry::EditableMesh* left,
    const geometry::EditableMesh* right) {
    if (left == nullptr || right == nullptr) return left == right;
    if (left->revision() != right->revision() ||
        left->vertex_count() != right->vertex_count() ||
        left->face_count() != right->face_count() ||
        left->has_attribute_payload() != right->has_attribute_payload()) {
        return false;
    }
    const auto left_vertices = left->vertices_sorted();
    const auto right_vertices = right->vertices_sorted();
    for (std::size_t index = 0U; index < left_vertices.size(); ++index) {
        if (left_vertices[index].id != right_vertices[index].id ||
            left_vertices[index].position.x != right_vertices[index].position.x ||
            left_vertices[index].position.y != right_vertices[index].position.y ||
            left_vertices[index].position.z != right_vertices[index].position.z) {
            return false;
        }
    }
    const auto left_faces = left->faces_sorted();
    const auto right_faces = right->faces_sorted();
    for (std::size_t index = 0U; index < left_faces.size(); ++index) {
        if (left_faces[index].id != right_faces[index].id ||
            left_faces[index].vertices != right_faces[index].vertices) {
            return false;
        }
    }
    if (left->attributes().domain_counts() != right->attributes().domain_counts() ||
        left->attributes().layers().size() != right->attributes().layers().size() ||
        left->uv_sets().descriptors().size() != right->uv_sets().descriptors().size()) {
        return false;
    }
    for (const auto& [id, layer] : left->attributes().layers()) {
        const auto found = right->attributes().layers().find(id);
        if (found == right->attributes().layers().end() ||
            layer.descriptor.id != found->second.descriptor.id ||
            layer.descriptor.domain != found->second.descriptor.domain ||
            layer.descriptor.type != found->second.descriptor.type ||
            layer.values.size() != found->second.values.size()) {
            return false;
        }
        for (std::size_t index = 0U; index < layer.values.size(); ++index) {
            const auto& a = layer.values[index];
            const auto& b = found->second.values[index];
            if (a.index() != b.index()) return false;
            const bool equal = std::visit([](const auto& first, const auto& second) {
                using First = std::decay_t<decltype(first)>;
                using Second = std::decay_t<decltype(second)>;
                if constexpr (!std::is_same_v<First, Second>) {
                    return false;
                } else if constexpr (std::is_same_v<First, core::Vec2d>) {
                    return first.x == second.x && first.y == second.y;
                } else if constexpr (std::is_same_v<First, core::Vec3d>) {
                    return first.x == second.x && first.y == second.y && first.z == second.z;
                } else if constexpr (std::is_same_v<First, core::Vec4d>) {
                    return first.x == second.x && first.y == second.y &&
                        first.z == second.z && first.w == second.w;
                } else {
                    return first == second;
                }
            }, a, b);
            if (!equal) return false;
        }
    }
    for (const auto& [id, descriptor] : left->uv_sets().descriptors()) {
        const auto found = right->uv_sets().descriptors().find(id);
        if (found == right->uv_sets().descriptors().end() ||
            descriptor.id != found->second.id || descriptor.name != found->second.name ||
            descriptor.layer_id != found->second.layer_id ||
            descriptor.active_for_editing != found->second.active_for_editing ||
            descriptor.active_for_render != found->second.active_for_render ||
            descriptor.lightmap_candidate != found->second.lightmap_candidate ||
            descriptor.tile_policy_version != found->second.tile_policy_version) {
            return false;
        }
    }
    return true;
}

assets::Sha256Digest rebase_set_digest(
    std::string_view label,
    const std::vector<scene::ObjectId>& objects,
    const std::vector<std::uint64_t>& mesh_assets) {
    std::string canonical = "cartographer.agent.rebase-set.v1|";
    append_field(canonical, label);
    append_number(canonical, objects.size());
    for (const auto object : objects) append_number(canonical, object.value);
    append_number(canonical, mesh_assets.size());
    for (const auto mesh_asset : mesh_assets) append_number(canonical, mesh_asset);
    return digest_text(canonical);
}

assets::Sha256Digest ghost_correction_chain_digest(
    const std::vector<GhostCorrectionReceipt>& corrections) {
    std::string canonical = "cartographer.agent.ghost-correction-chain.v1|";
    append_number(canonical, corrections.size());
    for (const auto& correction : corrections) {
        append_field(canonical, correction.evidence_digest.hex());
    }
    return digest_text(canonical);
}

std::string effect_names(std::uint32_t effects) {
    std::string result;
    const auto append = [&result](std::string_view name) {
        if (!result.empty()) result += ", ";
        result += name;
    };
    if ((effects & ::carto::agent::effect_mask(Effect::geometry)) != 0U) append("geometry");
    if ((effects & ::carto::agent::effect_mask(Effect::topology)) != 0U) append("topology");
    if ((effects & ::carto::agent::effect_mask(Effect::transform)) != 0U) append("transform");
    return result.empty() ? "no declared effect" : result;
}

std::vector<std::string> objective_tokens(std::string_view text) {
    const auto is_digit = [](unsigned char value) {
        return value >= static_cast<unsigned char>('0') &&
            value <= static_cast<unsigned char>('9');
    };
    const auto is_ascii_alphanumeric = [&is_digit](unsigned char value) {
        return is_digit(value) ||
            (value >= static_cast<unsigned char>('a') &&
                value <= static_cast<unsigned char>('z')) ||
            (value >= static_cast<unsigned char>('A') &&
                value <= static_cast<unsigned char>('Z'));
    };
    std::vector<std::string> tokens;
    std::string current;
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        const bool decimal_point = text[index] == '.' && index > 0U &&
            index + 1U < text.size() &&
            is_digit(static_cast<unsigned char>(text[index - 1U])) &&
            is_digit(static_cast<unsigned char>(text[index + 1U]));
        if (is_ascii_alphanumeric(character) || decimal_point) {
            const auto lowercase = character >= static_cast<unsigned char>('A') &&
                character <= static_cast<unsigned char>('Z')
                ? static_cast<unsigned char>(character + ('a' - 'A')) : character;
            current.push_back(static_cast<char>(lowercase));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

bool contains_objective_phrase(
    const std::vector<std::string>& tokens,
    std::initializer_list<std::string_view> phrase) {
    if (phrase.size() == 0U || phrase.size() > tokens.size()) return false;
    for (std::size_t start = 0U; start + phrase.size() <= tokens.size(); ++start) {
        std::size_t offset = 0U;
        bool matches = true;
        for (const auto word : phrase) {
            if (tokens[start + offset] != word) {
                matches = false;
                break;
            }
            ++offset;
        }
        if (matches) return true;
    }
    return false;
}

void consume_objective_phrase(
    const std::vector<std::string>& tokens,
    std::vector<bool>& consumed,
    std::initializer_list<std::string_view> phrase) {
    if (phrase.size() == 0U || phrase.size() > tokens.size()) return;
    for (std::size_t start = 0U; start + phrase.size() <= tokens.size(); ++start) {
        std::size_t offset = 0U;
        bool matches = true;
        for (const auto word : phrase) {
            if (tokens[start + offset] != word) {
                matches = false;
                break;
            }
            ++offset;
        }
        if (!matches) continue;
        for (std::size_t index = start; index < start + phrase.size(); ++index) {
            consumed[index] = true;
        }
        start += phrase.size() - 1U;
    }
}

bool has_uncompiled_restriction(
    const std::vector<std::string>& tokens,
    const std::vector<bool>& consumed) {
    for (std::size_t index = 0U; index < tokens.size(); ++index) {
        if (consumed[index]) continue;
        const auto& token = tokens[index];
        if (token == "do" && index + 1U < tokens.size() && tokens[index + 1U] == "not") {
            return true;
        }
        if (token == "don" && index + 1U < tokens.size() && tokens[index + 1U] == "t") {
            return true;
        }
        if (token == "must" && index + 1U < tokens.size() && tokens[index + 1U] == "not") {
            return true;
        }
        if (token == "no" || token == "never" || token == "avoid" ||
            token == "without" || token == "preserve" || token == "protect" ||
            token == "protected" || token == "untouched" || token == "unchanged" ||
            token == "keep" || token == "unless" || token == "except" ||
            token == "if" || token == "not") {
            return true;
        }
    }
    return false;
}

bool has_uncompiled_topology_change(
    const std::vector<std::string>& tokens,
    const std::vector<bool>& consumed) {
    for (std::size_t start = 0U; start < tokens.size(); ++start) {
        std::size_t end = start;
        if (tokens[start] == "change" || tokens[start] == "changes") {
            end = start + 1U;
            if (end < tokens.size() &&
                (tokens[end] == "the" || tokens[end] == "existing" ||
                    tokens[end] == "current")) {
                ++end;
            }
            if (end >= tokens.size() || tokens[end] != "topology") continue;
            ++end;
        } else if (tokens[start] == "topology" && start + 1U < tokens.size() &&
            (tokens[start + 1U] == "change" || tokens[start + 1U] == "changes" ||
                tokens[start + 1U] == "changed")) {
            end = start + 2U;
        } else {
            continue;
        }
        if (std::any_of(consumed.begin() + static_cast<std::ptrdiff_t>(start),
                consumed.begin() + static_cast<std::ptrdiff_t>(end),
                [](bool is_consumed) { return !is_consumed; })) {
            return true;
        }
    }
    return false;
}

} // namespace

core::Result<void> AgentDemonstrationParameters::validate(std::string_view tool_id) const {
    const bool displacement_is_zero = relative_displacement.x == 0.0 &&
        relative_displacement.y == 0.0 && relative_displacement.z == 0.0;
    switch (parameterization) {
    case AgentDemonstrationParameterization::normalized_face_distance:
        if ((tool_id != "mesh.inset-face" && tool_id != "mesh.extrude-face") ||
            !std::isfinite(normalized_distance) || normalized_distance <= 0.0 ||
            normalized_distance > 1'000'000.0 || !displacement_is_zero) {
            return core::Result<void>::failure(invalid(
                "demonstration face-distance parameter is invalid for its native tool"));
        }
        return core::Result<void>::success();
    case AgentDemonstrationParameterization::relative_displacement: {
        const double magnitude = relative_displacement.length();
        if ((tool_id != "object.translate" && tool_id != "mesh.set-vertex-position") ||
            !relative_displacement.finite() || !std::isfinite(magnitude) ||
            !(magnitude > 0.0) || magnitude > 1'000'000.0 || normalized_distance != 0.0) {
            return core::Result<void>::failure(invalid(
                "demonstration displacement parameter is invalid for its native tool"));
        }
        return core::Result<void>::success();
    }
    }
    return core::Result<void>::failure(invalid(
        "demonstration parameterization kind is unknown"));
}

const std::vector<AgentRecipeDefinition>& agent_recipe_catalog() noexcept {
    static const std::vector<AgentRecipeDefinition> recipes{
        {
            "bounded-mesh-edit",
            "Bounded mesh edit",
            "Uses the operator-authored plan, caps topology changes at 512 operations, "
                "and requires scene-object identity and transforms to remain unchanged.",
            512U,
            ::carto::agent::effect_mask(Effect::geometry) |
                ::carto::agent::effect_mask(Effect::topology),
            false,
        },
        {
            "bounded-vertex-edit",
            "Topology-preserving vertex edit",
            "Uses the operator-authored plan, permits vertex geometry only, and requires mesh topology and scene-object identity to remain unchanged.",
            0U,
            ::carto::agent::effect_mask(Effect::geometry),
            true,
        },
    };
    return recipes;
}

const AgentRecipeDefinition* find_agent_recipe(std::string_view id) noexcept {
    const auto& recipes = agent_recipe_catalog();
    const auto found = std::find_if(recipes.begin(), recipes.end(), [id](const auto& recipe) {
        return recipe.id == id;
    });
    return found == recipes.end() ? nullptr : &*found;
}

core::Result<std::vector<HardConstraint>> compile_explicit_hard_constraints(
    std::string_view goal) {
    if (!safe_text(goal, kMaxGoalBytes)) {
        return core::Result<std::vector<HardConstraint>>::failure(invalid(
            "objective goal is empty, malformed, or exceeds its text bound"));
    }
    const auto tokens = objective_tokens(goal);
    const bool preserves_topology =
        contains_objective_phrase(tokens, {"do", "not", "change", "topology"}) ||
        contains_objective_phrase(tokens, {"don", "t", "change", "topology"}) ||
        contains_objective_phrase(tokens, {"no", "topology", "changes"}) ||
        contains_objective_phrase(tokens, {"no", "changes", "to", "topology"}) ||
        contains_objective_phrase(tokens, {"keep", "topology", "unchanged"}) ||
        contains_objective_phrase(tokens, {"preserve", "topology"}) ||
        contains_objective_phrase(tokens, {"preserve", "existing", "topology"}) ||
        contains_objective_phrase(tokens, {"without", "changing", "topology"}) ||
        contains_objective_phrase(tokens, {"topology", "must", "remain", "unchanged"}) ||
        contains_objective_phrase(tokens, {"topology", "should", "not", "change"});
    const bool permits_topology_change =
        contains_objective_phrase(tokens, {"do", "not", "preserve", "topology"}) ||
        contains_objective_phrase(tokens, {"don", "t", "preserve", "topology"}) ||
        contains_objective_phrase(tokens, {"allow", "topology", "changes"}) ||
        contains_objective_phrase(tokens,
            {"allow", "topology", "changes", "if", "needed"}) ||
        contains_objective_phrase(tokens, {"allow", "changes", "to", "topology"}) ||
        contains_objective_phrase(tokens, {"permit", "topology", "change"}) ||
        contains_objective_phrase(tokens, {"permit", "topology", "changes"}) ||
        contains_objective_phrase(tokens, {"permit", "changes", "to", "topology"}) ||
        contains_objective_phrase(tokens, {"topology", "may", "change"}) ||
        contains_objective_phrase(tokens, {"topology", "can", "change"}) ||
        contains_objective_phrase(tokens, {"topology", "could", "change"}) ||
        contains_objective_phrase(tokens, {"topology", "might", "change"}) ||
        contains_objective_phrase(tokens, {"topology", "is", "allowed", "to", "change"}) ||
        contains_objective_phrase(tokens, {"change", "topology", "if", "needed"});
    if (preserves_topology && permits_topology_change) {
        return core::Result<std::vector<HardConstraint>>::failure(invalid(
            "objective gives conflicting topology requirements"));
    }

    std::vector<bool> consumed(tokens.size(), false);
    consume_objective_phrase(tokens, consumed, {"do", "not", "preserve", "topology"});
    consume_objective_phrase(tokens, consumed, {"don", "t", "preserve", "topology"});
    consume_objective_phrase(tokens, consumed, {"allow", "topology", "changes"});
    consume_objective_phrase(tokens, consumed,
        {"allow", "topology", "changes", "if", "needed"});
    consume_objective_phrase(tokens, consumed, {"allow", "changes", "to", "topology"});
    consume_objective_phrase(tokens, consumed, {"permit", "topology", "change"});
    consume_objective_phrase(tokens, consumed, {"permit", "topology", "changes"});
    consume_objective_phrase(tokens, consumed, {"permit", "changes", "to", "topology"});
    consume_objective_phrase(tokens, consumed, {"topology", "may", "change"});
    consume_objective_phrase(tokens, consumed, {"topology", "can", "change"});
    consume_objective_phrase(tokens, consumed, {"topology", "could", "change"});
    consume_objective_phrase(tokens, consumed, {"topology", "might", "change"});
    consume_objective_phrase(tokens, consumed,
        {"topology", "is", "allowed", "to", "change"});
    consume_objective_phrase(tokens, consumed, {"change", "topology", "if", "needed"});
    consume_objective_phrase(tokens, consumed, {"do", "not", "change", "topology"});
    consume_objective_phrase(tokens, consumed, {"don", "t", "change", "topology"});
    consume_objective_phrase(tokens, consumed, {"no", "topology", "changes"});
    consume_objective_phrase(tokens, consumed, {"no", "changes", "to", "topology"});
    consume_objective_phrase(tokens, consumed, {"keep", "topology", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"preserve", "topology"});
    consume_objective_phrase(tokens, consumed, {"preserve", "existing", "topology"});
    consume_objective_phrase(tokens, consumed, {"without", "changing", "topology"});
    consume_objective_phrase(tokens, consumed, {"topology", "must", "remain", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"topology", "should", "not", "change"});

    if (preserves_topology && has_uncompiled_topology_change(tokens, consumed)) {
        return core::Result<std::vector<HardConstraint>>::failure(invalid(
            "objective gives conflicting topology requirements"));
    }
    if (has_uncompiled_restriction(tokens, consumed)) {
        return core::Result<std::vector<HardConstraint>>::failure(invalid(
            "objective contains a restriction this compiler cannot enforce; revise the goal"));
    }

    std::vector<HardConstraint> constraints;
    if (preserves_topology) {
        constraints.push_back(HardConstraint{
            "compiled.preserve-topology",
            ObjectiveConstraintKind::preserve_topology,
            std::nullopt,
            0.0,
            "Compiled from explicit goal language requiring unchanged topology."});
    }
    for (const auto& constraint : constraints) {
        if (auto result = constraint.validate(); !result) {
            return core::Result<std::vector<HardConstraint>>::failure(result.error());
        }
    }
    return core::Result<std::vector<HardConstraint>>::success(std::move(constraints));
}

core::Result<void> TargetRef::validate() const {
    if (!object) return core::Result<void>::failure(invalid("plan target object is zero"));
    const bool valid_component = std::visit([](const auto value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return true;
        else return static_cast<bool>(value);
    }, component);
    if (!valid_component) {
        return core::Result<void>::failure(invalid("plan target component is zero"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> GhostCorrectionReceipt::computed_evidence_digest() const {
    if (!safe_text(task_id, kMaxTaskIdBytes) || !object ||
        !before_transform.finite() || !after_transform.finite() ||
        same_transform(before_transform, after_transform) || source_revision.exhausted() ||
        branch_revision_before.exhausted() ||
        branch_revision_after != branch_revision_before.next() || plan_context_digest.is_zero() ||
        !safe_text(justification, 1024U)) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "ghost correction evidence is incomplete or outside its bounds"));
    }
    std::string canonical = "cartographer.agent.ghost-correction.v1|";
    append_field(canonical, task_id);
    append_number(canonical, object.value);
    const auto append_transform = [&canonical](const core::Transform& transform) {
        append_double(canonical, transform.translation.x);
        append_double(canonical, transform.translation.y);
        append_double(canonical, transform.translation.z);
        append_double(canonical, transform.rotation.x);
        append_double(canonical, transform.rotation.y);
        append_double(canonical, transform.rotation.z);
        append_double(canonical, transform.rotation.w);
        append_double(canonical, transform.scale.x);
        append_double(canonical, transform.scale.y);
        append_double(canonical, transform.scale.z);
    };
    append_transform(before_transform);
    append_transform(after_transform);
    append_number(canonical, source_revision.value());
    append_number(canonical, branch_revision_before.value());
    append_number(canonical, branch_revision_after.value());
    append_field(canonical, plan_context_digest.hex());
    append_field(canonical, parent_evidence_digest.hex());
    append_field(canonical, justification);
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical));
}

core::Result<void> GhostCorrectionReceipt::validate() const {
    const auto computed = computed_evidence_digest();
    if (!computed) return core::Result<void>::failure(computed.error());
    if (evidence_digest.is_zero() || evidence_digest != computed.value()) {
        return core::Result<void>::failure(invalid(
            "ghost correction evidence digest does not bind its operator action"));
    }
    return core::Result<void>::success();
}

core::Result<void> HardConstraint::validate() const {
    if (!safe_text(id, kMaxStepIdBytes) || !safe_text(rationale, 512U) ||
        (object.has_value() && !*object)) {
        return core::Result<void>::failure(invalid("objective hard constraint is invalid"));
    }
    switch (kind) {
    case ObjectiveConstraintKind::preserve_topology:
        if (maximum_displacement != 0.0) {
            return core::Result<void>::failure(invalid(
                "topology-preservation constraint cannot declare a displacement bound"));
        }
        break;
    case ObjectiveConstraintKind::preserve_transform:
        if (!object.has_value() || maximum_displacement != 0.0) {
            return core::Result<void>::failure(invalid(
                "transform-preservation constraint requires one object and no numeric bound"));
        }
        break;
    case ObjectiveConstraintKind::maximum_vertex_displacement:
        if (!object.has_value() || !std::isfinite(maximum_displacement) ||
            maximum_displacement <= 0.0) {
            return core::Result<void>::failure(invalid(
                "vertex-displacement constraint requires one object and a finite positive bound"));
        }
        break;
    default:
        return core::Result<void>::failure(invalid(
            "objective hard constraint uses an unknown constraint kind"));
    }
    return core::Result<void>::success();
}

core::Result<void> ClarificationQuestion::validate(
    const std::vector<scene::ObjectId>& scope,
    const std::vector<HardConstraint>& constraints) const {
    if (!safe_text(id, kMaxStepIdBytes) || !safe_text(prompt, 512U) ||
        !safe_text(rationale, 512U) || impacts.empty() || impacts.size() > 9U ||
        affected_objects.size() > kMaxScopeObjects ||
        affected_constraint_ids.size() > 128U || downstream_plan_branches > kMaxPlanSteps ||
        candidates.size() > 16U) {
        return core::Result<void>::failure(invalid("clarification question is invalid"));
    }
    std::set<ClarificationImpact> unique_impacts;
    for (const auto impact : impacts) {
        if (clarification_impact_weight(impact) == 0U || !unique_impacts.insert(impact).second) {
            return core::Result<void>::failure(invalid(
                "clarification question contains an unknown or duplicate impact"));
        }
    }
    const auto validate_scoped_ids = [&scope](
        const std::vector<scene::ObjectId>& objects, std::string_view label) {
        if (auto result = validate_object_ids(objects, label, true); !result) return result;
        for (const auto object_id : objects) {
            if (!contains(scope, object_id)) {
                return core::Result<void>::failure(policy(
                    std::string(label) + " contains an object outside the objective scope"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_scoped_ids(affected_objects, "clarification affected objects");
        !result) return result;

    std::set<std::string> known_constraints;
    for (const auto& constraint : constraints) known_constraints.insert(constraint.id);
    std::set<std::string> unique_constraint_ids;
    for (const auto& constraint_id : affected_constraint_ids) {
        if (!safe_text(constraint_id, kMaxStepIdBytes) ||
            !unique_constraint_ids.insert(constraint_id).second ||
            !known_constraints.contains(constraint_id)) {
            return core::Result<void>::failure(invalid(
                "clarification question references an unknown or duplicate constraint"));
        }
    }

    std::set<std::string> candidate_ids;
    for (const auto& candidate : candidates) {
        if (!safe_text(candidate.id, kMaxStepIdBytes) || !safe_text(candidate.label, 256U) ||
            !candidate_ids.insert(candidate.id).second) {
            return core::Result<void>::failure(invalid(
                "clarification candidate identity or label is invalid"));
        }
        if (auto result = validate_scoped_ids(
                candidate.affected_objects, "clarification candidate objects"); !result) {
            return result;
        }
    }
    if (selected_candidate_id.has_value() &&
        (!candidate_ids.contains(*selected_candidate_id) || freeform_answer.has_value())) {
        return core::Result<void>::failure(invalid(
            "clarification answer does not match one candidate choice"));
    }
    if (freeform_answer.has_value() && !safe_text(*freeform_answer, 1024U)) {
        return core::Result<void>::failure(invalid("clarification answer is invalid"));
    }
    if ((selected_candidate_id.has_value() || freeform_answer.has_value()) !=
        human_receipt.has_value()) {
        return core::Result<void>::failure(invalid(
            "clarification answers require a receipt from the human application boundary"));
    }
    return core::Result<void>::success();
}

bool ClarificationFrontier::requires_user_input() const noexcept {
    return std::any_of(questions.begin(), questions.end(), [this](const auto& question) {
        return question.impact_score >= threshold;
    });
}

core::Result<void> AuthoringObjective::validate() const {
    if (!safe_text(id, kMaxTaskIdBytes) || !safe_text(goal, kMaxGoalBytes)) {
        return core::Result<void>::failure(invalid("objective identity or goal is invalid"));
    }
    auto compiled_constraints = compile_explicit_hard_constraints(goal);
    if (!compiled_constraints) {
        return core::Result<void>::failure(compiled_constraints.error());
    }
    if (!recipe_id.empty() && find_agent_recipe(recipe_id) == nullptr) {
        return core::Result<void>::failure(invalid(
            "objective references an unknown agent recipe"));
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
    if (hard_constraints.size() > 128U || clarifications.size() > 128U) {
        return core::Result<void>::failure(invalid(
            "objective constraint or clarification count exceeds its limit"));
    }
    std::set<std::string> constraint_ids;
    for (const auto& constraint : hard_constraints) {
        if (auto result = constraint.validate(); !result) return result;
        if (!constraint_ids.insert(constraint.id).second ||
            (constraint.object.has_value() && !contains(scope_objects, *constraint.object))) {
            return core::Result<void>::failure(invalid(
                "objective constraints contain duplicate identity or an out-of-scope target"));
        }
    }
    for (const auto& required : compiled_constraints.value()) {
        const bool represented = std::any_of(
            hard_constraints.begin(), hard_constraints.end(), [&required](const auto& existing) {
                return existing.kind == required.kind &&
                    existing.object == required.object &&
                    existing.maximum_displacement == required.maximum_displacement;
            });
        if (!represented) {
            return core::Result<void>::failure(invalid(
                "objective omits a hard constraint required by explicit goal language"));
        }
    }
    std::set<std::string> clarification_ids;
    for (const auto& question : clarifications) {
        if (auto result = question.validate(scope_objects, hard_constraints); !result) return result;
        if (!clarification_ids.insert(question.id).second) {
            return core::Result<void>::failure(invalid(
                "objective contains duplicate clarification identity"));
        }
        if (question.human_receipt.has_value()) {
            const auto& receipt = *question.human_receipt;
            if (receipt.question_id_ != question.id ||
                receipt.context_digest_ != ::carto::agent::clarification_context_digest(
                    *this) ||
                receipt.answer_digest_ != ::carto::agent::clarification_answer_digest(question)) {
                return core::Result<void>::failure(stale(
                    "human clarification receipt does not match the current question or answer"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> AuthoringObjective::digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::string canonical = "cartographer.authoring-objective.v1|";
    append_field(canonical, id);
    append_field(canonical, goal);
    if (!recipe_id.empty()) {
        append_field(canonical, "recipe");
        append_field(canonical, recipe_id);
    }
    append_number(canonical, base_revision.value());
    append_object_ids(canonical, scope_objects);
    append_object_ids(canonical, protected_objects);

    auto constraints = hard_constraints;
    std::sort(constraints.begin(), constraints.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    append_number(canonical, constraints.size());
    for (const auto& constraint : constraints) {
        append_field(canonical, constraint.id);
        append_number(canonical, static_cast<std::uint64_t>(constraint.kind));
        append_number(canonical, constraint.object.has_value() ? 1U : 0U);
        if (constraint.object.has_value()) append_number(canonical, constraint.object->value);
        append_double(canonical, constraint.maximum_displacement);
        append_field(canonical, constraint.rationale);
    }

    auto questions = clarifications;
    std::sort(questions.begin(), questions.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    append_number(canonical, questions.size());
    for (const auto& question : questions) {
        append_field(canonical, question.id);
        append_field(canonical, question.prompt);
        append_field(canonical, question.rationale);
        append_number(canonical, question.requires_explicit_answer ? 1U : 0U);
        auto impacts = question.impacts;
        std::sort(impacts.begin(), impacts.end());
        append_number(canonical, impacts.size());
        for (const auto impact : impacts) {
            append_number(canonical, static_cast<std::uint64_t>(impact));
        }
        append_object_ids(canonical, question.affected_objects);
        auto affected_constraints = question.affected_constraint_ids;
        std::sort(affected_constraints.begin(), affected_constraints.end());
        append_number(canonical, affected_constraints.size());
        for (const auto& constraint_id : affected_constraints) append_field(canonical, constraint_id);
        append_number(canonical, question.downstream_plan_branches);
        auto candidates = question.candidates;
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        append_number(canonical, candidates.size());
        for (const auto& candidate : candidates) {
            append_field(canonical, candidate.id);
            append_field(canonical, candidate.label);
            append_object_ids(canonical, candidate.affected_objects);
        }
        append_number(canonical, question.selected_candidate_id.has_value() ? 1U : 0U);
        if (question.selected_candidate_id.has_value()) {
            append_field(canonical, *question.selected_candidate_id);
        }
        append_number(canonical, question.freeform_answer.has_value() ? 1U : 0U);
        if (question.freeform_answer.has_value()) append_field(canonical, *question.freeform_answer);
        append_number(canonical, question.human_receipt.has_value() ? 1U : 0U);
        if (question.human_receipt.has_value()) {
            append_field(canonical, question.human_receipt->question_id_);
            append_field(canonical, question.human_receipt->context_digest_.hex());
            append_field(canonical, question.human_receipt->answer_digest_.hex());
            append_field(canonical, question.human_receipt->plan_context_digest_.hex());
        }
    }
    return core::Result<assets::Sha256Digest>::success(assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size())));
}

core::Result<assets::Sha256Digest> AuthoringObjective::clarification_context_digest(
    std::string_view question_id) const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const auto question = std::find_if(clarifications.begin(), clarifications.end(),
        [question_id](const auto& value) { return value.id == question_id; });
    if (question == clarifications.end()) {
        return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "clarification identity is absent from the objective"));
    }
    return core::Result<assets::Sha256Digest>::success(
        ::carto::agent::clarification_context_digest(*this));
}

core::Result<assets::Sha256Digest> AuthoringObjective::clarification_answer_digest(
    std::string_view question_id) const {
    const auto question = std::find_if(clarifications.begin(), clarifications.end(),
        [question_id](const auto& value) { return value.id == question_id; });
    if (question == clarifications.end() ||
        (!question->selected_candidate_id.has_value() && !question->freeform_answer.has_value())) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "clarification answer is absent from the objective"));
    }
    return core::Result<assets::Sha256Digest>::success(
        ::carto::agent::clarification_answer_digest(*question));
}

core::Result<ClarificationFrontier> AuthoringObjective::clarification_frontier(
    std::uint32_t threshold) const {
    if (threshold == 0U || threshold > 100U) {
        return core::Result<ClarificationFrontier>::failure(invalid(
            "clarification threshold must be between one and one hundred"));
    }
    if (auto result = validate(); !result) {
        return core::Result<ClarificationFrontier>::failure(result.error());
    }
    ClarificationFrontier frontier;
    frontier.threshold = threshold;
    for (const auto& question : clarifications) {
        if (question.selected_candidate_id.has_value() || question.freeform_answer.has_value()) {
            continue;
        }
        std::uint32_t score = 0U;
        for (const auto impact : question.impacts) score += clarification_impact_weight(impact);
        score += static_cast<std::uint32_t>(std::min<std::size_t>(
            question.downstream_plan_branches, 4U) * 5U);
        if (question.requires_explicit_answer) score = std::max(score, threshold);
        frontier.questions.push_back(RankedClarification{
            question, std::min(score, 100U)});
    }
    std::sort(frontier.questions.begin(), frontier.questions.end(), [](const auto& left, const auto& right) {
        if (left.impact_score != right.impact_score) return left.impact_score > right.impact_score;
        return left.question.id < right.question.id;
    });
    return core::Result<ClarificationFrontier>::success(std::move(frontier));
}

core::Result<void> AgentPlanStep::validate() const {
    if (!safe_text(id, kMaxStepIdBytes) || !safe_text(intent, kMaxStepIntentBytes)) {
        return core::Result<void>::failure(invalid("plan step identity or intent is invalid"));
    }
    if (dependencies.size() > kMaxPlanSteps) {
        return core::Result<void>::failure(invalid(
            "plan step dependency count exceeds its limit"));
    }
    std::set<std::string> unique_dependencies;
    for (const auto& dependency : dependencies) {
        if (!safe_text(dependency, kMaxStepIdBytes) ||
            !unique_dependencies.insert(dependency).second) {
            return core::Result<void>::failure(invalid(
                "plan step contains an invalid or duplicate dependency"));
        }
    }
    return target.validate();
}

core::Result<std::vector<std::string>> AgentPlan::schedule_order() const {
    if (steps.empty() || steps.size() > kMaxPlanSteps) {
        return core::Result<std::vector<std::string>>::failure(invalid(
            "plan step count is outside its supported range"));
    }

    std::map<std::string, std::size_t> indices;
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        if (!safe_text(steps[index].id, kMaxStepIdBytes) ||
            !indices.emplace(steps[index].id, index).second) {
            return core::Result<std::vector<std::string>>::failure(invalid(
                "plan contains an invalid or duplicate step identity"));
        }
    }

    std::vector<std::size_t> indegree(steps.size(), 0U);
    std::vector<std::vector<std::size_t>> dependents(steps.size());
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        if (steps[index].dependencies.size() > kMaxPlanSteps) {
            return core::Result<std::vector<std::string>>::failure(invalid(
                "plan step dependency count exceeds its limit"));
        }
        std::set<std::string> unique_dependencies;
        for (const auto& dependency : steps[index].dependencies) {
            const auto found = indices.find(dependency);
            if (!safe_text(dependency, kMaxStepIdBytes) ||
                !unique_dependencies.insert(dependency).second) {
                return core::Result<std::vector<std::string>>::failure(invalid(
                    "plan contains an invalid or duplicate dependency"));
            }
            if (found == indices.end()) {
                return core::Result<std::vector<std::string>>::failure(invalid(
                    "plan dependency does not name a step in this plan"));
            }
            if (found->second == index) {
                return core::Result<std::vector<std::string>>::failure(invalid(
                    "plan step cannot depend on itself"));
            }
            ++indegree[index];
            dependents[found->second].push_back(index);
        }
    }

    std::priority_queue<
        std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> ready;
    for (std::size_t index = 0U; index < indegree.size(); ++index) {
        if (indegree[index] == 0U) ready.push(index);
    }
    std::vector<std::string> order;
    order.reserve(steps.size());
    while (!ready.empty()) {
        const std::size_t current = ready.top();
        ready.pop();
        order.push_back(steps[current].id);
        for (const std::size_t dependent : dependents[current]) {
            if (--indegree[dependent] == 0U) ready.push(dependent);
        }
    }
    if (order.size() != steps.size()) {
        return core::Result<std::vector<std::string>>::failure(invalid(
            "plan dependency graph contains a cycle"));
    }
    return core::Result<std::vector<std::string>>::success(std::move(order));
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
    if (auto order = schedule_order(); !order) {
        return core::Result<void>::failure(order.error());
    }
    const auto plan_context_digest = clarification_plan_context_digest(*this);
    for (const auto& question : objective.clarifications) {
        if (question.human_receipt.has_value() &&
            question.human_receipt->plan_context_digest_ != plan_context_digest) {
            return core::Result<void>::failure(stale(
                "human clarification receipt does not match the current plan steps"));
        }
        if (!question.selected_candidate_id.has_value()) continue;
        const auto candidate = std::find_if(
            question.candidates.begin(), question.candidates.end(), [&](const auto& value) {
                return value.id == *question.selected_candidate_id;
            });
        if (candidate == question.candidates.end() || candidate->affected_objects.empty()) continue;
        for (const auto& step : steps) {
            if (contains(question.affected_objects, step.target.object) &&
                !contains(candidate->affected_objects, step.target.object)) {
                return core::Result<void>::failure(policy(
                    "plan target contradicts the selected clarification candidate"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> AgentPlan::context_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(
        clarification_plan_context_digest(*this));
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
    if (max_topology_operations > 1'000'000U ||
        max_repair_attempts > kMaxRepairAttempts ||
        max_additional_operations > kMaxPlanSteps) {
        return core::Result<void>::failure(invalid(
            "work envelope repair budget exceeds its supported bound"));
    }
    const bool permits_transform =
        (permitted_effects & ::carto::agent::effect_mask(Effect::transform)) != 0U;
    if (!std::isfinite(max_transform_distance_meters) ||
        max_transform_distance_meters < 0.0 || max_transform_distance_meters > 1'000'000.0 ||
        (permits_transform && max_transform_distance_meters <= 0.0) ||
        (!permits_transform && max_transform_distance_meters != 0.0)) {
        return core::Result<void>::failure(invalid(
            "work envelope transform permission requires a finite positive distance bound"));
    }
    SceneQueryBudget query_budget;
    query_budget.max_queries = max_scene_queries;
    query_budget.max_objects_examined = max_objects_examined;
    query_budget.max_topology_elements_examined = max_topology_elements_examined;
    query_budget.max_semantic_records_examined = max_semantic_records_examined;
    query_budget.max_result_bytes = max_query_result_bytes;
    if (auto result = query_budget.validate(); !result) return result;
    return core::Result<void>::success();
}

std::size_t ProjectDiff::changed_object_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        objects.begin(), objects.end(), has_changes));
}

std::size_t ProjectDiff::changed_mesh_count() const noexcept {
    return meshes.size();
}

core::Result<void> AgentRebaseReceipt::validate() const {
    const auto ordered_objects = [](const std::vector<scene::ObjectId>& values) {
        return std::all_of(values.begin(), values.end(), [](scene::ObjectId value) {
                return static_cast<bool>(value);
            }) && std::is_sorted(values.begin(), values.end()) &&
            std::adjacent_find(values.begin(), values.end()) == values.end();
    };
    const auto ordered_assets = [](const std::vector<std::uint64_t>& values) {
        return std::all_of(values.begin(), values.end(), [](std::uint64_t value) {
                return value != 0U;
            }) && std::is_sorted(values.begin(), values.end()) &&
            std::adjacent_find(values.begin(), values.end()) == values.end();
    };
    const auto subset = [](const auto& smaller, const auto& larger) {
        return std::all_of(smaller.begin(), smaller.end(), [&larger](const auto value) {
            return std::binary_search(larger.begin(), larger.end(), value);
        });
    };
    if (!safe_text(checkpoint_task_id, kMaxTaskIdBytes) ||
        previous_source_revision.exhausted() ||
        rebased_source_revision <= previous_source_revision ||
        checkpoint_source_digest.is_zero() || rebased_source_digest.is_zero() ||
        read_set_digest.is_zero() || write_set_digest.is_zero() ||
        read_objects.size() > kMaxScopeObjects || write_objects.size() > kMaxScopeObjects ||
        read_mesh_assets.size() > kMaxScopeObjects ||
        write_mesh_assets.size() > kMaxScopeObjects ||
        !ordered_objects(read_objects) || !ordered_objects(write_objects) ||
        !ordered_assets(read_mesh_assets) || !ordered_assets(write_mesh_assets) ||
        !subset(write_objects, read_objects) || !subset(write_mesh_assets, read_mesh_assets) ||
        rebase_set_digest("read", read_objects, read_mesh_assets) != read_set_digest ||
        rebase_set_digest("write", write_objects, write_mesh_assets) != write_set_digest) {
        return core::Result<void>::failure(invalid(
            "agent rebase receipt is malformed or its read/write sets are not bound"));
    }
    return core::Result<void>::success();
}

AgentTask::AgentTask(
    application::ApplicationSession& source,
    AgentPlan plan,
    AgentWorkEnvelope envelope,
    std::unique_ptr<application::ApplicationSession> speculative,
    application::ApplicationSnapshot source_snapshot,
    std::vector<AgentRepairReceipt> repairs,
    std::vector<AgentRebaseReceipt> rebases)
    : source_(&source),
      plan_(std::move(plan)),
      envelope_(std::move(envelope)),
      base_document_(source.document_),
      base_scene_(std::move(source_snapshot.viewport.scene)),
      base_viewport_error_(std::move(source_snapshot.viewport.error)),
      speculative_(std::move(speculative)),
      repairs_(std::move(repairs)),
      rebases_(std::move(rebases)) {
    plan_progress_.reserve(plan_.steps.size());
    for (const auto& step : plan_.steps) {
        plan_progress_.push_back(PlanStepProgress{
            step.id, step.dependencies, StepStatus::pending, std::nullopt});
    }
    query_budget_.max_queries = envelope_.max_scene_queries;
    query_budget_.max_objects_examined = envelope_.max_objects_examined;
    query_budget_.max_topology_elements_examined =
        envelope_.max_topology_elements_examined;
    query_budget_.max_semantic_records_examined = envelope_.max_semantic_records_examined;
    query_budget_.max_result_bytes = envelope_.max_query_result_bytes;
}

core::Result<AgentTask> AgentTask::begin(
    application::ApplicationSession& source,
    AgentPlan plan,
    AgentWorkEnvelope envelope) {
    if (auto result = plan.validate(); !result) {
        return core::Result<AgentTask>::failure(result.error());
    }
    auto frontier = plan.objective.clarification_frontier();
    if (!frontier) return core::Result<AgentTask>::failure(frontier.error());
    const auto pending_question = std::find_if(
        frontier.value().questions.begin(), frontier.value().questions.end(),
        [&frontier](const RankedClarification& question) {
            return question.impact_score >= frontier.value().threshold;
        });
    if (pending_question != frontier.value().questions.end()) {
        return core::Result<AgentTask>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "objective requires clarification before planning: " +
                pending_question->question.id + " — " + pending_question->question.prompt));
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<AgentTask>::failure(result.error());
    }
    if (const auto* recipe = find_agent_recipe(plan.objective.recipe_id);
        recipe != nullptr) {
        if ((envelope.permitted_effects & agent::effect_mask(Effect::transform)) != 0U) {
            return core::Result<AgentTask>::failure(policy(
                "selected agent recipe does not permit object transforms"));
        }
        envelope.permitted_effects &= recipe->permitted_effects;
        envelope.max_topology_operations = std::min(
            envelope.max_topology_operations, recipe->max_topology_operations);
        if (auto result = envelope.validate(); !result) {
            return core::Result<AgentTask>::failure(result.error());
        }
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

core::Result<AgentTask> AgentTask::repair(
    application::ApplicationSession& source,
    const AgentTask& failed_attempt,
    AgentPlan revised_plan,
    AgentWorkEnvelope envelope,
    std::string justification) {
    const bool failed_required_gate = std::any_of(
        failed_attempt.gates_.begin(), failed_attempt.gates_.end(),
        [](const AcceptanceGateResult& gate) { return gate.required && !gate.passed; });
    const bool repairable_status = failed_attempt.status_ == TaskStatus::blocked ||
        (failed_attempt.status_ == TaskStatus::review && failed_required_gate);
    if (failed_attempt.source_ != &source || !repairable_status) {
        return core::Result<AgentTask>::failure(invalid_state(
            "repair requires a blocked attempt or failed required gate from the same source session"));
    }
    if (source.snapshot().project_revision !=
        failed_attempt.plan_.objective.base_revision) {
        return core::Result<AgentTask>::failure(stale(
            "repair source changed after the original task was prepared"));
    }
    if (!safe_text(justification, kMaxGoalBytes)) {
        return core::Result<AgentTask>::failure(invalid(
            "repair requires a bounded non-empty operator justification"));
    }
    if (const auto* recipe = find_agent_recipe(revised_plan.objective.recipe_id);
        recipe != nullptr) {
        if ((envelope.permitted_effects & agent::effect_mask(Effect::transform)) != 0U) {
            return core::Result<AgentTask>::failure(policy(
                "selected agent recipe does not permit object transforms"));
        }
        envelope.permitted_effects &= recipe->permitted_effects;
        envelope.max_topology_operations = std::min(
            envelope.max_topology_operations, recipe->max_topology_operations);
        if (auto result = envelope.validate(); !result) {
            return core::Result<AgentTask>::failure(result.error());
        }
    }
    if (failed_attempt.repairs_.size() >= envelope.max_repair_attempts) {
        return core::Result<AgentTask>::failure(policy(
            "work envelope repair-attempt budget is exhausted"));
    }
    if (envelope.scope_objects.size() > failed_attempt.envelope_.scope_objects.size() ||
        !std::all_of(envelope.scope_objects.begin(), envelope.scope_objects.end(),
            [&failed_attempt](scene::ObjectId object) {
                return contains(failed_attempt.envelope_.scope_objects, object);
            }) ||
        (envelope.permitted_effects & ~failed_attempt.envelope_.permitted_effects) != 0U ||
        envelope.max_commands > failed_attempt.envelope_.max_commands ||
        envelope.max_objects_touched > failed_attempt.envelope_.max_objects_touched ||
        envelope.max_topology_operations >
            failed_attempt.envelope_.max_topology_operations ||
        envelope.max_scene_queries > failed_attempt.envelope_.max_scene_queries ||
        envelope.max_objects_examined > failed_attempt.envelope_.max_objects_examined ||
        envelope.max_topology_elements_examined >
            failed_attempt.envelope_.max_topology_elements_examined ||
        envelope.max_semantic_records_examined >
            failed_attempt.envelope_.max_semantic_records_examined ||
        envelope.max_query_result_bytes > failed_attempt.envelope_.max_query_result_bytes ||
        envelope.max_repair_attempts > failed_attempt.envelope_.max_repair_attempts ||
        envelope.max_additional_operations >
            failed_attempt.envelope_.max_additional_operations ||
        envelope.max_transform_distance_meters >
            failed_attempt.envelope_.max_transform_distance_meters) {
        return core::Result<AgentTask>::failure(policy(
            "repair cannot expand the previous work envelope"));
    }
    if (revised_plan.id == failed_attempt.plan_.id) {
        return core::Result<AgentTask>::failure(invalid(
            "repaired plan must receive a new task identity"));
    }
    for (const auto& repair : failed_attempt.repairs_) {
        if (revised_plan.id == repair.previous_task_id ||
            revised_plan.id == repair.repaired_task_id) {
            return core::Result<AgentTask>::failure(invalid(
                "repaired plan task identity was already used in this repair chain"));
        }
    }
    const auto previous_objective_digest = failed_attempt.plan_.objective.digest();
    const auto revised_objective_digest = revised_plan.objective.digest();
    if (!previous_objective_digest || !revised_objective_digest ||
        clarification_context_digest(failed_attempt.plan_.objective) !=
            clarification_context_digest(revised_plan.objective)) {
        return core::Result<AgentTask>::failure(policy(
            "repair must preserve the original objective, scope, constraints, and question context"));
    }
    if (revised_plan.objective.base_revision != failed_attempt.plan_.objective.base_revision) {
        return core::Result<AgentTask>::failure(stale(
            "repair cannot change the source revision of the failed attempt"));
    }
    auto revised_steps_only = revised_plan;
    revised_steps_only.id = failed_attempt.plan_.id;
    const auto previous_plan_digest = clarification_plan_context_digest(failed_attempt.plan_);
    const auto revised_plan_digest = clarification_plan_context_digest(revised_steps_only);
    if (previous_plan_digest == revised_plan_digest) {
        return core::Result<AgentTask>::failure(invalid(
            "repair must revise at least one plan step or dependency"));
    }

    const std::size_t added_operations = revised_plan.steps.size() >
        failed_attempt.plan_.steps.size()
        ? revised_plan.steps.size() - failed_attempt.plan_.steps.size() : 0U;
    std::size_t total_additional_operations = added_operations;
    for (const auto& repair : failed_attempt.repairs_) {
        if (repair.repaired_plan_steps > repair.previous_plan_steps) {
            const auto added = repair.repaired_plan_steps - repair.previous_plan_steps;
            if (added > kMaxPlanSteps - total_additional_operations) {
                return core::Result<AgentTask>::failure(policy(
                    "work envelope additional-operation budget is exhausted"));
            }
            total_additional_operations += added;
        }
    }
    if (total_additional_operations > envelope.max_additional_operations) {
        return core::Result<AgentTask>::failure(policy(
            "work envelope additional-operation budget is exhausted"));
    }

    std::string failure_canonical = "cartographer.agent-repair-failure.v1|";
    append_field(failure_canonical, failed_attempt.plan_.id);
    append_field(failure_canonical, previous_plan_digest.hex());
    append_number(failure_canonical, failed_attempt.plan_.objective.base_revision.value());
    append_number(failure_canonical, failed_attempt.receipts_.size());
    for (const auto& receipt : failed_attempt.receipts_) {
        append_field(failure_canonical, receipt.step_id);
        append_number(failure_canonical, static_cast<std::uint64_t>(receipt.status));
        append_number(failure_canonical, receipt.revision_before.value());
        append_number(failure_canonical, receipt.revision_after.value());
        append_number(failure_canonical, receipt.diagnostic.has_value() ? 1U : 0U);
        if (receipt.diagnostic.has_value()) {
            append_number(failure_canonical, static_cast<std::uint64_t>(receipt.diagnostic->code));
            append_field(failure_canonical, receipt.diagnostic->message);
        }
    }
    append_number(failure_canonical, failed_attempt.gates_.size());
    for (const auto& gate : failed_attempt.gates_) {
        append_field(failure_canonical, gate.id);
        append_number(failure_canonical, gate.passed ? 1U : 0U);
        append_field(failure_canonical, gate.evidence_digest.hex());
    }
    append_number(failure_canonical, failed_attempt.diagnostic_.has_value() ? 1U : 0U);
    if (failed_attempt.diagnostic_.has_value()) {
        append_number(failure_canonical,
            static_cast<std::uint64_t>(failed_attempt.diagnostic_->code));
        append_field(failure_canonical, failed_attempt.diagnostic_->message);
    }
    const auto failure_digest = digest_text(failure_canonical);

    std::string repair_canonical = "cartographer.agent-repair-evidence.v1|";
    append_number(repair_canonical, failed_attempt.repairs_.size() + 1U);
    append_field(repair_canonical, failed_attempt.plan_.id);
    append_field(repair_canonical, revised_plan.id);
    append_field(repair_canonical, justification);
    append_number(repair_canonical, failed_attempt.plan_.objective.base_revision.value());
    append_field(repair_canonical, previous_plan_digest.hex());
    append_field(repair_canonical, revised_plan_digest.hex());
    append_field(repair_canonical, previous_objective_digest.value().hex());
    append_field(repair_canonical, revised_objective_digest.value().hex());
    append_field(repair_canonical, failure_digest.hex());
    const auto previous_correction_digest =
        ghost_correction_chain_digest(failed_attempt.ghost_corrections_);
    append_number(repair_canonical, failed_attempt.ghost_corrections_.size());
    append_field(repair_canonical, previous_correction_digest.hex());

    AgentRepairReceipt repair_receipt;
    repair_receipt.attempt = failed_attempt.repairs_.size() + 1U;
    repair_receipt.previous_task_id = failed_attempt.plan_.id;
    repair_receipt.repaired_task_id = revised_plan.id;
    repair_receipt.justification = std::move(justification);
    repair_receipt.previous_plan_steps = failed_attempt.plan_.steps.size();
    repair_receipt.repaired_plan_steps = revised_plan.steps.size();
    repair_receipt.source_revision = failed_attempt.plan_.objective.base_revision;
    repair_receipt.previous_plan_digest = previous_plan_digest;
    repair_receipt.repaired_plan_digest = clarification_plan_context_digest(revised_plan);
    repair_receipt.previous_objective_digest = previous_objective_digest.value();
    repair_receipt.repaired_objective_digest = revised_objective_digest.value();
    repair_receipt.failure_evidence_digest = failure_digest;
    repair_receipt.repair_evidence_digest = digest_text(repair_canonical);
    repair_receipt.previous_ghost_correction_count =
        failed_attempt.ghost_corrections_.size();
    repair_receipt.previous_ghost_correction_digest = previous_correction_digest;
    repair_receipt.previous_step_receipts = failed_attempt.receipts_;
    repair_receipt.previous_gate_results = failed_attempt.gates_;

    auto next_attempt = begin(source, std::move(revised_plan), std::move(envelope));
    if (!next_attempt) return next_attempt;
    next_attempt.value().repairs_ = failed_attempt.repairs_;
    next_attempt.value().rebases_ = failed_attempt.rebases_;
    next_attempt.value().ghost_corrections_ = failed_attempt.ghost_corrections_;
    next_attempt.value().replay_ghost_corrections_ =
        !next_attempt.value().ghost_corrections_.empty();
    next_attempt.value().repairs_.push_back(std::move(repair_receipt));
    return core::Result<AgentTask>::success(std::move(next_attempt.value()));
}

bool AgentTask::scope_contains(scene::ObjectId object) const noexcept {
    return contains(envelope_.scope_objects, object);
}

core::Result<void> AgentTask::correct_ghost_transform(
    scene::ObjectId object,
    core::Transform transform,
    std::string justification) {
    if ((status_ != TaskStatus::review && status_ != TaskStatus::blocked) ||
        receipts_.empty() || !source_ || !speculative_) {
        return core::Result<void>::failure(invalid_state(
            "ghost corrections require an executed speculative task"));
    }
    if (source_->document_.revision() != plan_.objective.base_revision) {
        return core::Result<void>::failure(stale(
            "source project changed while the speculative result was under review"));
    }
    if (!contains(plan_.objective.scope_objects, object) || !scope_contains(object)) {
        return core::Result<void>::failure(policy(
            "ghost correction target is outside the declared task scope"));
    }
    if (contains(plan_.objective.protected_objects, object)) {
        return core::Result<void>::failure(policy(
            "ghost correction cannot change an object protected by the objective"));
    }
    if (!safe_text(justification, 1024U)) {
        return core::Result<void>::failure(invalid(
            "ghost correction requires a bounded non-empty operator justification"));
    }
    if (!transform.finite()) {
        return core::Result<void>::failure(invalid(
            "ghost correction transform must contain finite values and a valid rotation"));
    }
    if (ghost_corrections_.size() >= 256U) {
        return core::Result<void>::failure(policy(
            "ghost correction evidence budget is exhausted"));
    }
    const auto* current = speculative_->document_.scene().find(object);
    if (current == nullptr) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "ghost correction target is absent from the speculative branch"));
    }
    if (current->locked || current->parent.has_value()) {
        return core::Result<void>::failure(policy(
            "native ghost transform editing requires an unlocked root object"));
    }
    if (same_transform(current->local_transform, transform)) {
        return core::Result<void>::failure(invalid(
            "ghost correction does not change the selected transform"));
    }
    const auto plan_context_digest = plan_.context_digest();
    if (!plan_context_digest) {
        return core::Result<void>::failure(plan_context_digest.error());
    }

    GhostCorrectionReceipt correction;
    correction.task_id = plan_.id;
    correction.object = object;
    correction.before_transform = current->local_transform;
    correction.after_transform = transform;
    correction.source_revision = plan_.objective.base_revision;
    correction.branch_revision_before = speculative_->document_.revision();
    correction.branch_revision_after = correction.branch_revision_before.next();
    correction.plan_context_digest = plan_context_digest.value();
    correction.parent_evidence_digest = ghost_corrections_.empty()
        ? assets::Sha256Digest{} : ghost_corrections_.back().evidence_digest;
    correction.justification = std::move(justification);
    const auto evidence_digest = correction.computed_evidence_digest();
    if (!evidence_digest) return core::Result<void>::failure(evidence_digest.error());
    correction.evidence_digest = evidence_digest.value();

    const auto dispatched = application::HumanApplicationAccess::dispatch(
        *speculative_, application::SetObjectTransformAction{
            object, transform, correction.branch_revision_before});
    if (!dispatched) {
        return core::Result<void>::failure(dispatched.error().with_context(
            "human ghost correction"));
    }
    if (speculative_->document_.revision() != correction.branch_revision_after) {
        return core::Result<void>::failure(invalid_state(
            "ghost correction did not advance exactly one speculative revision"));
    }
    ghost_corrections_.push_back(std::move(correction));
    gates_ = evaluate_gates();
    const bool required_gates_pass = std::all_of(
        gates_.begin(), gates_.end(), [](const AcceptanceGateResult& gate) {
            return !gate.required || gate.passed;
        });
    const bool all_steps_passed = std::all_of(
        plan_progress_.begin(), plan_progress_.end(), [](const PlanStepProgress& progress) {
            return progress.status == StepStatus::passed;
        });
    if (all_steps_passed && required_gates_pass) {
        status_ = TaskStatus::review;
        diagnostic_.reset();
    } else {
        status_ = TaskStatus::blocked;
        diagnostic_ = invalid_state(
            "human ghost correction leaves one or more required acceptance gates blocked");
    }
    cached_snapshot_.reset();
    return core::Result<void>::success();
}

core::Result<void> AgentTask::apply_ghost_corrections() {
    if (ghost_corrections_.empty()) {
        replay_ghost_corrections_ = false;
        return core::Result<void>::success();
    }
    const auto current_plan_context = plan_.context_digest();
    if (!current_plan_context) {
        return core::Result<void>::failure(current_plan_context.error());
    }
    assets::Sha256Digest expected_parent;
    std::map<scene::ObjectId, core::Transform> first_transforms;
    std::map<scene::ObjectId, core::Transform> final_transforms;
    std::map<scene::ObjectId, core::Transform> last_recorded_transforms;
    for (const auto& correction : ghost_corrections_) {
        const bool plan_context_matches =
            (correction.task_id == plan_.id &&
                correction.plan_context_digest == current_plan_context.value()) ||
            std::any_of(repairs_.begin(), repairs_.end(), [&correction](const auto& repair) {
                return (correction.task_id == repair.previous_task_id &&
                        correction.plan_context_digest == repair.previous_plan_digest) ||
                    (correction.task_id == repair.repaired_task_id &&
                        correction.plan_context_digest == repair.repaired_plan_digest);
            }) ||
            std::any_of(rebases_.begin(), rebases_.end(), [&correction](const auto& rebase) {
                return correction.task_id == rebase.checkpoint_task_id &&
                    correction.plan_context_digest ==
                        rebase.checkpoint_plan_context_digest;
            });
        const bool source_revision_matches =
            correction.source_revision == plan_.objective.base_revision ||
            std::any_of(rebases_.begin(), rebases_.end(), [&correction](const auto& rebase) {
                return correction.source_revision == rebase.previous_source_revision ||
                    correction.source_revision == rebase.rebased_source_revision;
            });
        if (auto valid = correction.validate(); !valid) return valid;
        if (correction.parent_evidence_digest != expected_parent ||
            !source_revision_matches ||
            !plan_context_matches ||
            !contains(plan_.objective.scope_objects, correction.object) ||
            !scope_contains(correction.object) ||
            contains(plan_.objective.protected_objects, correction.object)) {
            return core::Result<void>::failure(policy(
                "locked ghost correction lineage no longer matches the task scope"));
        }
        const auto previous = last_recorded_transforms.find(correction.object);
        if (previous != last_recorded_transforms.end() &&
            !same_transform(previous->second, correction.before_transform)) {
            return core::Result<void>::failure(stale(
                "locked ghost correction transform chain is discontinuous"));
        }
        first_transforms.try_emplace(correction.object, correction.before_transform);
        last_recorded_transforms[correction.object] = correction.after_transform;
        final_transforms[correction.object] = correction.after_transform;
        expected_parent = correction.evidence_digest;
    }

    for (const auto& [object, expected_transform] : first_transforms) {
        const auto* current = speculative_->document_.scene().find(object);
        if (current == nullptr || current->locked || current->parent.has_value()) {
            return core::Result<void>::failure(policy(
                "locked ghost correction target is missing, locked, or no longer a root object"));
        }
        if (!same_transform(current->local_transform, expected_transform)) {
            return core::Result<void>::failure(stale(
                "replayed plan changed the base transform for a locked ghost correction"));
        }
        const auto& final_transform = final_transforms.at(object);
        if (same_transform(current->local_transform, final_transform)) continue;
        const auto revision_before = speculative_->document_.revision();
        const auto dispatched = application::HumanApplicationAccess::dispatch(
            *speculative_, application::SetObjectTransformAction{
                object, final_transform, revision_before});
        if (!dispatched) {
            return core::Result<void>::failure(dispatched.error().with_context(
                "replaying locked ghost correction"));
        }
    }
    replay_ghost_corrections_ = false;
    return core::Result<void>::success();
}

core::Result<void> AgentTask::execute_step(const AgentPlanStep& step) {
    StepReceipt receipt;
    receipt.step_id = step.id;
    receipt.intent = step.intent;
    receipt.target_object = step.target.object;
    receipt.revision_before = speculative_->document_.revision();
    const auto fail = [this, &receipt](core::Diagnostic diagnostic) {
        receipt.status = StepStatus::failed;
        receipt.revision_after = speculative_->document_.revision();
        receipt.diagnostic = diagnostic;
        receipts_.push_back(std::move(receipt));
        return core::Result<void>::failure(std::move(diagnostic));
    };

    if (!scope_contains(step.target.object)) {
        return fail(policy("plan step target is outside the work envelope"));
    }
    const auto* target_object = speculative_->document_.scene().find(step.target.object);
    if (target_object == nullptr) {
        return fail(policy("plan target object no longer exists in the speculative branch"));
    }
    const bool object_translation =
        std::holds_alternative<std::monostate>(step.target.component);
    if (object_translation &&
        (envelope_.permitted_effects & ::carto::agent::effect_mask(Effect::transform)) == 0U) {
        return fail(policy("work envelope forbids the transform effect"));
    }
    std::set<scene::ObjectId> affected_objects;
    if (object_translation) {
        if (target_object->parent.has_value()) {
            return fail(policy("object translation requires a root object target"));
        }
        const auto scene_objects = speculative_->document_.scene().objects_sorted();
        for (const auto& object : scene_objects) {
            bool in_target_subtree = object.id == step.target.object;
            const scene::SceneObject* ancestor = &object;
            std::set<scene::ObjectId> visited;
            for (std::size_t depth = 0U; !in_target_subtree &&
                 ancestor != nullptr && ancestor->parent.has_value(); ++depth) {
                if (depth >= 4096U || !visited.insert(*ancestor->parent).second) {
                    return fail(policy("object translation encountered an invalid hierarchy"));
                }
                if (*ancestor->parent == step.target.object) {
                    in_target_subtree = true;
                    break;
                }
                ancestor = speculative_->document_.scene().find(*ancestor->parent);
                if (ancestor == nullptr) {
                    return fail(policy("object translation hierarchy has a missing parent"));
                }
            }
            if (!in_target_subtree) continue;
            if (!scope_contains(object.id)) {
                return fail(policy(
                    "object translation would affect a descendant outside the work envelope"));
            }
            if (contains(plan_.objective.protected_objects, object.id)) {
                return fail(policy(
                    "object translation would affect a protected descendant"));
            }
            if (object.locked) {
                return fail(policy("object translation would affect a locked descendant"));
            }
            affected_objects.insert(object.id);
        }
    } else {
        if (!target_object->mesh_asset.has_value()) {
            return fail(policy("mesh operation target has no authored mesh asset"));
        }
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
    }
    if (!affected_objects.contains(step.target.object)) {
        return fail(policy("plan target has no admitted affected object"));
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
    if (object_translation) {
        select_mode = application::SetSelectionModeAction{editor::SelectionMode::object};
        select_component = application::SelectObjectAction{step.target.object};
    } else if (const auto* face = std::get_if<geometry::FaceId>(&step.target.component)) {
        select_mode = application::SetSelectionModeAction{editor::SelectionMode::face};
        select_component = application::SelectFaceAction{step.target.object, *face};
    } else if (const auto* vertex =
                   std::get_if<geometry::VertexId>(&step.target.component)) {
        select_mode = application::SetSelectionModeAction{editor::SelectionMode::vertex};
        select_component = application::SelectVertexAction{step.target.object, *vertex};
    } else {
        return fail(invalid("plan target component is not a supported native operation target"));
    }
    auto selected = application::HumanApplicationAccess::dispatch(*speculative_, select_mode);
    if (!selected) return fail(selected.error().with_context("speculative plan selection mode"));
    selected = application::HumanApplicationAccess::dispatch(*speculative_, select_component);
    if (!selected) return fail(selected.error().with_context("speculative plan target selection"));

    const auto authoring = speculative_->authoring_context();
    if (!authoring) return fail(authoring.error().with_context("speculative authoring context"));
    const auto snapshot = speculative_->snapshot();
    const auto registry = ai::build_operation_ontology(snapshot.tools);
    const ai::Context context{
        snapshot.project_revision,
        authoring.value(),
        ai::project_capabilities(registry, envelope_.permitted_effects)};
    if (auto result = context.validate(); !result) {
        return fail(result.error().with_context("speculative capability context"));
    }
    auto proposal = planner_.propose(step.intent, context);
    if (!proposal) return fail(proposal.error().with_context("native plan step"));
    receipt.tool_id = proposal.value().tool_id;
    if (auto result = proposal.value().validate(context); !result) {
        return fail(result.error().with_context("native proposal validation"));
    }
    const auto capability = std::find_if(
        context.operations.begin(), context.operations.end(),
        [&proposal](const ai::OperationDescriptor& operation) {
            return operation.id == proposal.value().tool_id;
        });
    if (capability == context.operations.end() || !capability->provider_requestable) {
        return fail(policy("plan selected a capability without a governed effect contract"));
    }
    if ((capability->effects & ~envelope_.permitted_effects) != 0U) {
        return fail(policy(
            "work envelope forbids the " + effect_names(capability->effects) + " effect"));
    }
    double transform_step_distance = 0.0;
    if (object_translation) {
        if (!proposal.value().parameters.position.has_value()) {
            return fail(invalid_state("object translation proposal omitted its displacement"));
        }
        const auto displacement = *proposal.value().parameters.position;
        transform_step_distance = std::hypot(
            displacement.x, displacement.y, displacement.z);
        if (!std::isfinite(transform_step_distance) ||
            transform_distance_used_meters_ > envelope_.max_transform_distance_meters ||
            transform_step_distance >
                envelope_.max_transform_distance_meters - transform_distance_used_meters_) {
            return fail(policy("plan exceeds the work envelope transform-distance budget"));
        }
        const auto protected_transform = std::find_if(
            plan_.objective.hard_constraints.begin(),
            plan_.objective.hard_constraints.end(), [&](const HardConstraint& constraint) {
                return constraint.kind == ObjectiveConstraintKind::preserve_transform &&
                    (!constraint.object.has_value() ||
                     *constraint.object == step.target.object);
            });
        if (protected_transform != plan_.objective.hard_constraints.end()) {
            return fail(policy(
                "objective hard constraint " + protected_transform->id +
                " forbids object translation for this target"));
        }
    }
    if ((capability->effects & ::carto::agent::effect_mask(Effect::topology)) != 0U) {
        const auto topology_constraint = std::find_if(
            plan_.objective.hard_constraints.begin(),
            plan_.objective.hard_constraints.end(), [&](const HardConstraint& constraint) {
                return constraint.kind == ObjectiveConstraintKind::preserve_topology &&
                    (!constraint.object.has_value() ||
                     *constraint.object == step.target.object);
            });
        if (topology_constraint != plan_.objective.hard_constraints.end()) {
            return fail(policy(
                "objective hard constraint " + topology_constraint->id +
                " forbids topology changes for this target"));
        }
    }

    const auto parameterized = parameterize_demo_step(
        proposal.value(), speculative_->document_, step.target);
    if (parameterized) receipt.demonstration_parameters = parameterized.value();

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
    transform_distance_used_meters_ += transform_step_distance;
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
    const auto scheduled = plan_.schedule_order();
    if (!scheduled) {
        status_ = TaskStatus::blocked;
        diagnostic_ = scheduled.error();
        gates_ = evaluate_gates();
        return core::Result<AgentTaskSnapshot>::success(snapshot());
    }
    for (const auto& step_id : scheduled.value()) {
        const auto step_it = std::find_if(plan_.steps.begin(), plan_.steps.end(),
            [&step_id](const AgentPlanStep& step) { return step.id == step_id; });
        const auto progress_it = std::find_if(plan_progress_.begin(), plan_progress_.end(),
            [&step_id](const PlanStepProgress& progress) { return progress.step_id == step_id; });
        if (step_it == plan_.steps.end() || progress_it == plan_progress_.end()) {
            status_ = TaskStatus::blocked;
            diagnostic_ = invalid_state("scheduler could not resolve a validated plan step");
            break;
        }
        const auto failed_dependency = std::find_if(
            progress_it->dependencies.begin(), progress_it->dependencies.end(),
            [this](const std::string& dependency) {
                const auto state = std::find_if(
                    plan_progress_.begin(), plan_progress_.end(),
                    [&dependency](const PlanStepProgress& progress) {
                        return progress.step_id == dependency;
                    });
                return state == plan_progress_.end() || state->status != StepStatus::passed;
            });
        if (failed_dependency != progress_it->dependencies.end()) {
            progress_it->status = StepStatus::invalidated;
            progress_it->diagnostic = policy(
                "plan branch invalidated because dependency " + *failed_dependency +
                " did not pass");
            continue;
        }
        progress_it->status = StepStatus::ready;
        progress_it->status = StepStatus::running;
        const std::size_t receipt_count_before = receipts_.size();
        auto result = execute_step(*step_it);
        if (result) {
            progress_it->status = StepStatus::passed;
        } else {
            progress_it->status = StepStatus::failed;
            progress_it->diagnostic = result.error();
            if (receipt_count_before == receipts_.size()) {
                status_ = TaskStatus::blocked;
                diagnostic_ = invalid_state("failed plan step did not produce a receipt");
                break;
            }
            if (!diagnostic_.has_value()) diagnostic_ = result.error();
        }
    }
    if (status_ == TaskStatus::running && replay_ghost_corrections_) {
        if (auto corrected = apply_ghost_corrections(); !corrected) {
            status_ = TaskStatus::blocked;
            diagnostic_ = corrected.error();
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
        if (!diagnostic_.has_value()) {
            diagnostic_ = invalid_state("one or more required agent acceptance gates failed");
        }
    }
    return core::Result<AgentTaskSnapshot>::success(snapshot());
}

const project::ProjectDocument& AgentTask::scene_query_document() const {
    return speculative_->document_;
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
    const ProjectDiff diff = build_diff();
    const auto source_revision = base_document_.revision();
    const auto evaluated_revision = speculative_->document_.revision();
    constexpr std::string_view validator_version = "cartographer.agent.acceptance.v1";

    std::vector<scene::ObjectId> all_affected_objects;
    std::vector<std::uint64_t> all_affected_assets;
    for (const auto& change : diff.objects) all_affected_objects.push_back(change.id);
    for (const auto& step : plan_.steps) all_affected_objects.push_back(step.target.object);
    for (const auto& mesh : diff.meshes) all_affected_assets.push_back(mesh.asset_id);
    for (const auto& step : plan_.steps) {
        const auto* object = base_document_.scene().find(step.target.object);
        if (object != nullptr && object->mesh_asset.has_value()) {
            all_affected_assets.push_back(*object->mesh_asset);
        }
    }
    std::sort(all_affected_objects.begin(), all_affected_objects.end());
    all_affected_objects.erase(
        std::unique(all_affected_objects.begin(), all_affected_objects.end()),
        all_affected_objects.end());
    std::sort(all_affected_assets.begin(), all_affected_assets.end());
    all_affected_assets.erase(
        std::unique(all_affected_assets.begin(), all_affected_assets.end()),
        all_affected_assets.end());

    const auto make_gate = [&](
        std::string id,
        bool passed,
        std::string evidence,
        std::vector<scene::ObjectId> affected_objects,
        std::vector<std::uint64_t> affected_assets,
        std::optional<core::Diagnostic> diagnostic = std::nullopt) {
        std::sort(affected_objects.begin(), affected_objects.end());
        affected_objects.erase(
            std::unique(affected_objects.begin(), affected_objects.end()),
            affected_objects.end());
        std::sort(affected_assets.begin(), affected_assets.end());
        affected_assets.erase(
            std::unique(affected_assets.begin(), affected_assets.end()),
            affected_assets.end());
        const assets::Sha256Digest result_digest = diff.digest;
        evidence += " [result=" + result_digest.hex() + "]";

        std::string canonical = "cartographer.acceptance-evidence.v1|";
        append_field(canonical, id);
        append_field(canonical, validator_version);
        append_number(canonical, source_revision.value());
        append_number(canonical, evaluated_revision.value());
        append_number(canonical, passed ? 1U : 0U);
        append_field(canonical, evidence);
        append_field(canonical, result_digest.hex());
        append_number(canonical, affected_objects.size());
        for (const auto object : affected_objects) append_number(canonical, object.value);
        append_number(canonical, affected_assets.size());
        for (const auto asset_id : affected_assets) append_number(canonical, asset_id);
        append_number(canonical, diagnostic.has_value() ? 1U : 0U);
        if (diagnostic.has_value()) {
            append_number(canonical, static_cast<std::uint64_t>(diagnostic->code));
            append_field(canonical, diagnostic->message);
            append_number(canonical, diagnostic->context.size());
            for (const auto& context : diagnostic->context) append_field(canonical, context);
        }

        AcceptanceGateResult gate;
        gate.id = std::move(id);
        gate.passed = passed;
        gate.evidence = std::move(evidence);
        gate.validator_version = validator_version;
        gate.source_revision = source_revision;
        gate.evaluated_revision = evaluated_revision;
        gate.affected_objects = std::move(affected_objects);
        gate.affected_asset_ids = std::move(affected_assets);
        gate.result_digest = result_digest;
        gate.evidence_digest = digest_text(canonical);
        gate.diagnostic = std::move(diagnostic);
        return gate;
    };

    const auto project_validation = speculative_->document_.validate();
    gates.push_back(make_gate(
        "project.valid", static_cast<bool>(project_validation),
        project_validation ? "shadow ProjectDocument validation passed"
                           : project_validation.error().message,
        all_affected_objects, all_affected_assets,
        project_validation ? std::nullopt
                           : std::optional<core::Diagnostic>(project_validation.error())));
    const auto viewport = speculative_->snapshot().viewport;
    gates.push_back(make_gate(
        "viewport.compiles", !viewport.error.has_value(),
        viewport.error ? viewport.error->message : "shadow viewport compiled",
        all_affected_objects, all_affected_assets, viewport.error));
    const bool all_steps_passed = receipts_.size() == plan_.steps.size() &&
        std::all_of(receipts_.begin(), receipts_.end(), [](const StepReceipt& receipt) {
            return receipt.status == StepStatus::passed;
        });
    std::optional<core::Diagnostic> step_diagnostic;
    if (!all_steps_passed) {
        const auto failed = std::find_if(receipts_.begin(), receipts_.end(),
            [](const StepReceipt& receipt) {
                return receipt.status != StepStatus::passed;
            });
        if (failed != receipts_.end() && failed->diagnostic.has_value()) {
            step_diagnostic = failed->diagnostic;
        } else {
            step_diagnostic = invalid_state("one or more native plan steps did not pass");
        }
    }
    gates.push_back(make_gate(
        "plan.steps", all_steps_passed,
        std::to_string(receipts_.size()) + " of " + std::to_string(plan_.steps.size()) +
            " native steps passed",
        all_affected_objects, all_affected_assets, std::move(step_diagnostic)));
    const bool changed = diff.changed_object_count() != 0U ||
        diff.changed_mesh_count() != 0U || diff.engineering_changed || diff.condition_changed;
    gates.push_back(make_gate(
        "diff.non_empty", changed,
        changed ? "structured source diff contains changes" : "no source changes were produced",
        all_affected_objects, all_affected_assets,
        changed ? std::nullopt : std::optional<core::Diagnostic>(
            invalid_state("agent plan produced no source changes"))));
    std::size_t topology_operations = 0U;
    const auto add_topology_operation = [&topology_operations](std::size_t count) {
        const std::size_t max_value = std::numeric_limits<std::size_t>::max();
        topology_operations = count > max_value - topology_operations
            ? max_value : topology_operations + count;
    };
    for (const auto& mesh : diff.meshes) {
        if (mesh.added || mesh.removed) add_topology_operation(1U);
        for (const auto& vertex : mesh.vertices) {
            if (vertex.added || vertex.removed) add_topology_operation(1U);
        }
        for (const auto& face : mesh.faces) {
            if (face.added || face.removed || face.connectivity_changed) {
                add_topology_operation(1U);
            }
        }
    }
    const bool topology_budget_passed =
        topology_operations <= envelope_.max_topology_operations;
    gates.push_back(make_gate(
        "envelope.topology_operations", topology_budget_passed,
        std::to_string(topology_operations) + " topology operations / " +
            std::to_string(envelope_.max_topology_operations) + " permitted",
        all_affected_objects, all_affected_assets,
        topology_budget_passed ? std::nullopt : std::optional<core::Diagnostic>(
            policy("agent result exceeds the work envelope topology-operation budget"))));
    if (!plan_.objective.recipe_id.empty()) {
        const auto* recipe = find_agent_recipe(plan_.objective.recipe_id);
        const bool identity_preserved = recipe != nullptr &&
            std::all_of(diff.objects.begin(), diff.objects.end(), [](const ObjectChange& change) {
                return !change.added && !change.removed && !change.name_changed &&
                    !change.transform_changed && !change.parent_changed &&
                    !change.mesh_binding_changed && !change.visibility_changed &&
                    !change.lock_changed;
            });
        const std::string recipe_gate_id = recipe == nullptr
            ? "recipe.unknown.scene-identity"
            : "recipe." + std::string(recipe->id) + ".scene-identity";
        gates.push_back(make_gate(
            recipe_gate_id,
            identity_preserved,
            identity_preserved
                ? "scene object identity, transforms, and bindings remained unchanged"
                : "scene object identity, transforms, or bindings changed",
            all_affected_objects,
            all_affected_assets,
            identity_preserved ? std::nullopt : std::optional<core::Diagnostic>(policy(
                "selected recipe requires scene-object identity and transforms to remain unchanged"))));
        if (recipe != nullptr && recipe->preserve_topology) {
            const bool topology_preserved = std::all_of(
                diff.meshes.begin(), diff.meshes.end(), [](const MeshChange& mesh) {
                    return !mesh.added && !mesh.removed &&
                        std::none_of(mesh.vertices.begin(), mesh.vertices.end(),
                            [](const VertexValueChange& vertex) {
                                return vertex.added || vertex.removed;
                            }) &&
                        std::none_of(mesh.faces.begin(), mesh.faces.end(),
                            [](const FaceTopologyChange& face) {
                                return face.added || face.removed || face.connectivity_changed;
                            });
                });
            gates.push_back(make_gate(
                "recipe." + std::string(recipe->id) + ".topology-preservation",
                topology_preserved,
                topology_preserved
                    ? "mesh vertex and face topology remained unchanged"
                    : "mesh vertex or face topology changed",
                all_affected_objects,
                all_affected_assets,
                topology_preserved ? std::nullopt : std::optional<core::Diagnostic>(policy(
                    "selected recipe requires mesh topology to remain unchanged"))));
        }
    }
    for (const auto& constraint : plan_.objective.hard_constraints) {
        bool passed = true;
        std::string evidence;
        std::vector<scene::ObjectId> constraint_objects = all_affected_objects;
        std::vector<std::uint64_t> constraint_assets = all_affected_assets;
        if (constraint.object.has_value()) {
            constraint_objects = {*constraint.object};
            const auto* object = base_document_.scene().find(*constraint.object);
            constraint_assets.clear();
            if (object != nullptr && object->mesh_asset.has_value()) {
                constraint_assets.push_back(*object->mesh_asset);
            }
        }
        if (constraint.kind == ObjectiveConstraintKind::preserve_topology) {
            for (const auto& mesh : diff.meshes) {
                if (constraint.object.has_value()) {
                    const auto* object = base_document_.scene().find(*constraint.object);
                    if (object == nullptr || object->mesh_asset != mesh.asset_id) continue;
                }
                const bool topology_changed = mesh.added || mesh.removed ||
                    std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [](const auto& vertex) {
                        return vertex.added || vertex.removed;
                    }) || std::any_of(mesh.faces.begin(), mesh.faces.end(), [](const auto& face) {
                        return face.added || face.removed || face.connectivity_changed;
                    });
                if (topology_changed) {
                    passed = false;
                    break;
                }
            }
            evidence = passed ? "no vertex or face topology changed"
                              : "a vertex or face topology record changed";
        } else if (constraint.kind == ObjectiveConstraintKind::preserve_transform) {
            const auto before_transform = base_document_.scene().world_transform(*constraint.object);
            const auto after_transform = speculative_->document_.scene().world_transform(
                *constraint.object);
            passed = before_transform && after_transform &&
                same_transform(before_transform.value(), after_transform.value());
            evidence = passed ? "target world transform is unchanged"
                : "target world transform changed or could not be resolved";
        } else {
            const auto* before_object = base_document_.scene().find(*constraint.object);
            const auto* after_object = speculative_->document_.scene().find(*constraint.object);
            double maximum_observed = 0.0;
            bool measurement_complete = true;
            if (before_object == nullptr || after_object == nullptr ||
                before_object->mesh_asset != after_object->mesh_asset ||
                !before_object->mesh_asset.has_value()) {
                passed = false;
                measurement_complete = false;
            } else {
                const auto before_transform = base_document_.scene().world_transform(*constraint.object);
                const auto after_transform = speculative_->document_.scene().world_transform(
                    *constraint.object);
                const auto before_mesh = base_document_.meshes().find(*before_object->mesh_asset);
                const auto after_mesh = speculative_->document_.meshes().find(
                    *after_object->mesh_asset);
                if (!before_transform || !after_transform) {
                    passed = false;
                    measurement_complete = false;
                } else if (before_mesh == base_document_.meshes().end() ||
                    after_mesh == speculative_->document_.meshes().end()) {
                    passed = false;
                    measurement_complete = false;
                } else if (before_mesh->second.vertex_count() != after_mesh->second.vertex_count()) {
                    passed = false;
                    measurement_complete = false;
                } else if (before_mesh->second.vertex_count() >
                    envelope_.max_topology_elements_examined) {
                    passed = false;
                    measurement_complete = false;
                } else {
                    for (const auto& vertex : before_mesh->second.vertices_sorted()) {
                        const auto* after_vertex = after_mesh->second.find_vertex(vertex.id);
                        if (after_vertex == nullptr) {
                            passed = false;
                            measurement_complete = false;
                            break;
                        }
                        const double displacement = (transform_point(
                            after_transform.value(), after_vertex->position) - transform_point(
                            before_transform.value(), vertex.position)).length();
                        if (!std::isfinite(displacement)) {
                            passed = false;
                            measurement_complete = false;
                            break;
                        }
                        maximum_observed = std::max(maximum_observed, displacement);
                        if (displacement > constraint.maximum_displacement) passed = false;
                    }
                }
            }
            evidence = "maximum observed vertex displacement " +
                std::to_string(maximum_observed) + " / " +
                std::to_string(constraint.maximum_displacement) + " world units";
            if (!measurement_complete) {
                evidence += "; complete correspondence unavailable or measurement budget exceeded";
            }
        }
        auto constraint_diagnostic = passed ? std::nullopt :
            std::optional<core::Diagnostic>(invalid_state(
                "hard constraint " + constraint.id + " could not be proven"));
        gates.push_back(make_gate(
            "constraint." + constraint.id, passed, std::move(evidence),
            std::move(constraint_objects), std::move(constraint_assets),
            std::move(constraint_diagnostic)));
    }
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
    view.recipe_id = plan_.objective.recipe_id;
    if (const auto objective_digest = plan_.objective.digest(); objective_digest) {
        view.objective_digest = objective_digest.value();
    }
    view.hard_constraints = plan_.objective.hard_constraints;
    view.clarifications = plan_.objective.clarifications;
    view.scope_objects = plan_.objective.scope_objects;
    view.protected_objects = plan_.objective.protected_objects;
    view.base_revision = plan_.objective.base_revision;
    const auto speculative_snapshot = speculative_->snapshot();
    view.speculative_revision = speculative_snapshot.project_revision;
    view.plan_steps = plan_progress_;
    view.steps = receipts_;
    view.repairs = repairs_;
    view.rebases = rebases_;
    view.ghost_corrections = ghost_corrections_;
    view.repair_attempts_remaining = envelope_.max_repair_attempts > repairs_.size()
        ? envelope_.max_repair_attempts - repairs_.size() : 0U;
    view.gates = gates_;
    view.diff = build_diff();
    view.query_budget = query_budget_;
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
    const auto objective_digest = plan_.objective.digest();
    if (!objective_digest) {
        return core::Result<application::DispatchReceipt>::failure(objective_digest.error());
    }
    const std::string diff_digest = build_diff().digest.hex();
    const auto accepted = source_->accept_speculative_result(
        speculative_->document_, plan_.objective.base_revision, plan_.id,
        objective_digest.value().hex(), diff_digest);
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

core::Result<AgentDemonstration> AgentTask::create_demonstration() const {
    return AgentDemonstration::capture(snapshot());
}

core::Result<void> AgentDemonstration::save(const std::filesystem::path& path) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            "demonstration destination must name a file"));
    }
    const auto encoded = serialize();
    if (!encoded) return core::Result<void>::failure(encoded.error());

    std::error_code error;
    const auto destination_status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to inspect demonstration destination"));
    }
    if (std::filesystem::exists(destination_status)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration records are immutable; choose a new destination path"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        const auto parent_status = std::filesystem::status(parent, error);
        if (error || !std::filesystem::is_directory(parent_status)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "demonstration destination directory does not exist"));
        }
    }

    auto lock_path = path;
    lock_path += ".lock";
    if (!std::filesystem::create_directory(lock_path, error)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration destination is already being written"));
    }
    struct LockCleanup final {
        std::filesystem::path path;
        ~LockCleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } lock_cleanup{lock_path};

    auto temporary_path = path;
    temporary_path += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error, "unable to create demonstration temporary file"));
        }
        output.write(encoded.value().data(),
            static_cast<std::streamsize>(encoded.value().size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary_path, error);
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error, "unable to flush demonstration temporary file"));
        }
    }
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to publish immutable demonstration file"));
    }
    return core::Result<void>::success();
}

core::Result<AgentDemonstration> AgentDemonstration::load(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<AgentDemonstration>::failure(invalid(
            "demonstration source must name a file"));
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 2U * 1024U * 1024U) {
        return core::Result<AgentDemonstration>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration is unavailable or exceeds its size bound"));
    }
    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input) {
        return core::Result<AgentDemonstration>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to open demonstration file"));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        return core::Result<AgentDemonstration>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to read complete demonstration file"));
    }
    return AgentDemonstration::deserialize(bytes);
}
core::Result<void> AgentTask::save_checkpoint(
    const std::filesystem::path& path,
    std::string provider_identity) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            "checkpoint destination must name a file"));
    }
    if (status_ == TaskStatus::running) {
        return core::Result<void>::failure(invalid_state(
            "a running task cannot be checkpointed"));
    }
    const auto plan_context = plan_.context_digest();
    if (!plan_context) return core::Result<void>::failure(plan_context.error());

    AgentTaskCheckpoint checkpoint;
    checkpoint.status = status_;
    checkpoint.plan = plan_;
    for (auto& question : checkpoint.plan.objective.clarifications) {
        question.selected_candidate_id.reset();
        question.freeform_answer.reset();
        question.human_receipt.reset();
    }
    checkpoint.envelope = envelope_;
    checkpoint.provider_identity = std::move(provider_identity);
    checkpoint.source_revision = base_document_.revision();
    checkpoint.plan_context_digest = plan_context.value();
    checkpoint.source_project = base_document_.serialize();
    checkpoint.speculative_project = speculative_->document_.serialize();
    checkpoint.source_digest = digest_text(checkpoint.source_project);
    checkpoint.speculative_digest = digest_text(checkpoint.speculative_project);
    checkpoint.diff_digest = build_diff().digest;
    checkpoint.plan_progress = plan_progress_;
    checkpoint.step_receipts = receipts_;
    checkpoint.repair_lineage = repairs_;
    checkpoint.rebase_lineage = rebases_;
    checkpoint.ghost_corrections = ghost_corrections_;
    for (const auto& gate : gates_) {
        if (!checkpoint.last_validation_report.empty()) {
            checkpoint.last_validation_report.push_back('\n');
        }
        checkpoint.last_validation_report += gate.id;
        checkpoint.last_validation_report += gate.passed ? ": passed — " : ": blocked — ";
        checkpoint.last_validation_report += gate.evidence;
        if (gate.diagnostic.has_value()) {
            checkpoint.last_validation_report += " — ";
            checkpoint.last_validation_report += gate.diagnostic->message;
        }
    }
    if (diagnostic_.has_value()) {
        if (!checkpoint.last_validation_report.empty()) {
            checkpoint.last_validation_report.push_back('\n');
        }
        checkpoint.last_validation_report += "task: ";
        checkpoint.last_validation_report += diagnostic_->message;
    }
    const auto encoded = checkpoint.serialize();
    if (!encoded) return core::Result<void>::failure(encoded.error());

    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "checkpoint files are immutable; choose a new destination path"));
    }
    if (error) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to inspect checkpoint destination"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, error)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "checkpoint destination directory does not exist"));
    }
    if (error) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to inspect checkpoint directory"));
    }

    auto lock_path = path;
    lock_path += ".lock";
    if (!std::filesystem::create_directory(lock_path, error)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "checkpoint destination is already being written"));
    }
    struct LockCleanup final {
        std::filesystem::path path;
        ~LockCleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } lock_cleanup{lock_path};

    auto temporary_path = path;
    temporary_path += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error, "unable to create checkpoint temporary file"));
        }
        output.write(encoded.value().data(), static_cast<std::streamsize>(encoded.value().size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary_path, error);
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error, "unable to flush checkpoint temporary file"));
        }
    }
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to publish immutable checkpoint file"));
    }
    return core::Result<void>::success();
}

core::Result<AgentTaskCheckpoint> AgentTask::load_checkpoint(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<AgentTaskCheckpoint>::failure(invalid(
            "checkpoint source must name a file"));
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 512U * 1024U * 1024U) {
        return core::Result<AgentTaskCheckpoint>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "checkpoint is unavailable or exceeds its size bound"));
    }
    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input) {
        return core::Result<AgentTaskCheckpoint>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to open checkpoint file"));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return core::Result<AgentTaskCheckpoint>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "checkpoint file changed or was truncated while reading"));
    }
    return AgentTaskCheckpoint::deserialize(bytes);
}

core::Result<AgentTask> AgentTask::resume_checkpoint(
    application::ApplicationSession& source,
    const AgentTaskCheckpoint& checkpoint,
    AgentPlan reauthorized_plan) {
    if (auto result = checkpoint.validate(); !result) {
        return core::Result<AgentTask>::failure(result.error());
    }
    if (checkpoint.status == TaskStatus::accepted ||
        checkpoint.status == TaskStatus::rejected) {
        return core::Result<AgentTask>::failure(invalid_state(
            "accepted and rejected tasks are historical records and cannot resume"));
    }
    const auto source_revision = source.document_.revision();
    if (source_revision != checkpoint.source_revision ||
        digest_text(source.document_.serialize()) != checkpoint.source_digest) {
        return core::Result<AgentTask>::failure(stale(
            "checkpoint source advanced or changed; compute and review source changes before rebase"));
    }
    const auto resumed_context = reauthorized_plan.context_digest();
    if (!resumed_context || resumed_context.value() != checkpoint.plan_context_digest) {
        return core::Result<AgentTask>::failure(stale(
            "resume plan differs from the checkpoint; replan and review before resuming"));
    }
    if (reauthorized_plan.objective.base_revision != source_revision) {
        return core::Result<AgentTask>::failure(stale(
            "resume objective is bound to a different source revision"));
    }

    auto resumed = begin(source, std::move(reauthorized_plan), checkpoint.envelope);
    if (!resumed) return resumed;
    resumed.value().repairs_ = checkpoint.repair_lineage;
    resumed.value().rebases_ = checkpoint.rebase_lineage;
    resumed.value().ghost_corrections_ = checkpoint.ghost_corrections;
    resumed.value().replay_ghost_corrections_ =
        !resumed.value().ghost_corrections_.empty();
    const auto replayed = resumed.value().run();
    if (!replayed) return core::Result<AgentTask>::failure(replayed.error());
    if (checkpoint.status != TaskStatus::prepared) {
        const auto replayed_project = resumed.value().speculative_->document_.serialize();
        const auto replayed_diff = resumed.value().build_diff().digest;
        if (digest_text(replayed_project) != checkpoint.speculative_digest ||
            replayed_diff != checkpoint.diff_digest) {
            resumed.value().status_ = TaskStatus::blocked;
            resumed.value().diagnostic_ = stale(
                "checkpoint replay differs from the saved speculative artifact or diff; acceptance is blocked");
            resumed.value().gates_ = resumed.value().evaluate_gates();
            resumed.value().cached_snapshot_.reset();
        }
    }
    return resumed;
}

core::Result<AgentRebaseAnalysis> AgentTask::analyze_checkpoint_rebase(
    const application::ApplicationSession& source,
    const AgentTaskCheckpoint& checkpoint) {
    if (auto result = checkpoint.validate(); !result) {
        return core::Result<AgentRebaseAnalysis>::failure(result.error());
    }
    auto base_result = project::ProjectDocument::deserialize(checkpoint.source_project);
    if (!base_result) {
        return core::Result<AgentRebaseAnalysis>::failure(base_result.error());
    }
    const auto& base = base_result.value();
    const auto& current = source.document_;
    AgentRebaseAnalysis analysis;
    analysis.checkpoint_revision = checkpoint.source_revision;
    analysis.current_revision = current.revision();
    analysis.current_source_digest = project_result_digest(current);

    std::set<scene::ObjectId> read_objects;
    std::set<scene::ObjectId> write_objects;
    std::set<std::uint64_t> read_mesh_assets;
    std::set<std::uint64_t> write_mesh_assets;
    const auto add_blocker = [&analysis](std::string message) {
        if (analysis.blockers.size() < 512U &&
            std::find(analysis.blockers.begin(), analysis.blockers.end(), message) ==
                analysis.blockers.end()) {
            analysis.blockers.push_back(std::move(message));
        }
    };
    const auto add_mesh_ref = [&read_mesh_assets](
        const project::ProjectDocument& document, scene::ObjectId object) {
        const auto* found = document.scene().find(object);
        if (found != nullptr && found->mesh_asset.has_value()) {
            read_mesh_assets.insert(*found->mesh_asset);
        }
    };
    const auto add_read_ancestry = [&read_objects, &add_blocker](
        const project::ProjectDocument& document, scene::ObjectId object) {
        std::set<scene::ObjectId> visited;
        const scene::SceneObject* current_object = document.scene().find(object);
        std::size_t depth = 0U;
        while (current_object != nullptr && current_object->parent.has_value()) {
            const scene::ObjectId parent = *current_object->parent;
            if (!visited.insert(parent).second || ++depth > 4096U) {
                add_blocker("checkpoint scope ancestry is cyclic or exceeds its analysis bound");
                return;
            }
            read_objects.insert(parent);
            current_object = document.scene().find(parent);
            if (current_object == nullptr) {
                add_blocker("checkpoint scope ancestry references a missing parent object");
                return;
            }
        }
    };

    if (checkpoint.status == TaskStatus::accepted || checkpoint.status == TaskStatus::rejected) {
        add_blocker("accepted and rejected task checkpoints are historical and cannot rebase");
    }
    if (analysis.current_revision < checkpoint.source_revision) {
        add_blocker("current source revision predates the checkpoint revision");
    } else if (analysis.current_revision == checkpoint.source_revision &&
        analysis.current_source_digest != checkpoint.source_digest) {
        add_blocker("source content changed without advancing its revision");
    }
    if (auto valid = current.validate(); !valid) {
        add_blocker("current source project no longer passes document validation");
    }

    for (const scene::ObjectId object : checkpoint.plan.objective.scope_objects) {
        read_objects.insert(object);
        if (base.scene().find(object) == nullptr) {
            add_blocker("checkpoint scope object " + std::to_string(object.value) +
                " is absent from its saved source");
            continue;
        }
        add_mesh_ref(base, object);
        add_read_ancestry(base, object);
    }
    for (const auto& step : checkpoint.plan.steps) {
        write_objects.insert(step.target.object);
        const auto* target = base.scene().find(step.target.object);
        if (target == nullptr) {
            add_blocker("checkpoint write target " + std::to_string(step.target.object.value) +
                " is absent from its saved source");
            continue;
        }
        if (std::holds_alternative<std::monostate>(step.target.component)) {
            for (const auto& object : base.scene().objects_sorted()) {
                const scene::SceneObject* ancestor = &object;
                std::set<scene::ObjectId> visited;
                for (std::size_t depth = 0U; ancestor != nullptr &&
                     ancestor->parent.has_value(); ++depth) {
                    const scene::ObjectId parent = *ancestor->parent;
                    if (depth >= 4096U || !visited.insert(parent).second) {
                        add_blocker("checkpoint object-transform hierarchy is invalid");
                        break;
                    }
                    if (parent == step.target.object) {
                        write_objects.insert(object.id);
                        break;
                    }
                    ancestor = base.scene().find(parent);
                    if (ancestor == nullptr) {
                        add_blocker("checkpoint object-transform hierarchy has a missing parent");
                    }
                }
            }
            continue;
        }
        if (!target->mesh_asset.has_value()) {
            add_blocker("checkpoint write target " + std::to_string(step.target.object.value) +
                " has no saved mesh binding");
            continue;
        }
        read_mesh_assets.insert(*target->mesh_asset);
        write_mesh_assets.insert(*target->mesh_asset);
    }
    for (const auto& correction : checkpoint.ghost_corrections) {
        write_objects.insert(correction.object);
    }
    for (const auto object : write_objects) add_read_ancestry(base, object);

    std::set<scene::ObjectId> object_ids;
    for (const auto& object : base.scene().objects_sorted()) object_ids.insert(object.id);
    for (const auto& object : current.scene().objects_sorted()) object_ids.insert(object.id);
    for (const scene::ObjectId object : object_ids) {
        const auto* before = base.scene().find(object);
        const auto* after = current.scene().find(object);
        if (same_scene_object(before, after)) continue;
        const bool read_conflict = read_objects.contains(object);
        const bool asset_membership_changed = before == nullptr || after == nullptr ||
            before->mesh_asset != after->mesh_asset;
        const bool shared_write_asset = asset_membership_changed &&
            ((before != nullptr && before->mesh_asset.has_value() &&
                write_mesh_assets.contains(*before->mesh_asset)) ||
             (after != nullptr && after->mesh_asset.has_value() &&
                write_mesh_assets.contains(*after->mesh_asset)));
        if (read_conflict || shared_write_asset) {
            analysis.conflicting_objects.push_back(object);
            if (shared_write_asset) {
                const auto add_conflicting_asset = [&analysis](std::uint64_t asset) {
                    analysis.conflicting_mesh_assets.push_back(asset);
                };
                if (before != nullptr && before->mesh_asset.has_value() &&
                    write_mesh_assets.contains(*before->mesh_asset)) {
                    add_conflicting_asset(*before->mesh_asset);
                }
                if (after != nullptr && after->mesh_asset.has_value() &&
                    write_mesh_assets.contains(*after->mesh_asset)) {
                    add_conflicting_asset(*after->mesh_asset);
                }
                add_blocker("object " + std::to_string(object.value) +
                    " changed its membership in a mesh asset written by the task");
            }
            if (read_conflict) {
                add_blocker("read object " + std::to_string(object.value) +
                    " changed since the checkpoint source");
            }
        }
    }

    std::set<std::uint64_t> mesh_ids;
    for (const auto& [id, mesh] : base.meshes()) {
        static_cast<void>(mesh);
        mesh_ids.insert(id);
    }
    for (const auto& [id, mesh] : current.meshes()) {
        static_cast<void>(mesh);
        mesh_ids.insert(id);
    }
    for (const std::uint64_t asset : mesh_ids) {
        const auto before = base.meshes().find(asset);
        const auto after = current.meshes().find(asset);
        const auto* before_mesh = before == base.meshes().end() ? nullptr : &before->second;
        const auto* after_mesh = after == current.meshes().end() ? nullptr : &after->second;
        if (same_mesh_state(before_mesh, after_mesh) || !read_mesh_assets.contains(asset)) continue;
        analysis.conflicting_mesh_assets.push_back(asset);
        add_blocker("read mesh asset " + std::to_string(asset) +
            " changed since the checkpoint source");
    }
    if (base.condition_authoring() != current.condition_authoring() ||
        base.engineering_authoring() != current.engineering_authoring()) {
        add_blocker("CONDITION or engineering sidecar changed; checkpoint has no field-level semantic read provenance");
    }

    analysis.read_objects.assign(read_objects.begin(), read_objects.end());
    analysis.write_objects.assign(write_objects.begin(), write_objects.end());
    analysis.read_mesh_assets.assign(read_mesh_assets.begin(), read_mesh_assets.end());
    analysis.write_mesh_assets.assign(write_mesh_assets.begin(), write_mesh_assets.end());
    std::sort(analysis.conflicting_objects.begin(), analysis.conflicting_objects.end());
    analysis.conflicting_objects.erase(std::unique(
        analysis.conflicting_objects.begin(), analysis.conflicting_objects.end()),
        analysis.conflicting_objects.end());
    std::sort(analysis.conflicting_mesh_assets.begin(), analysis.conflicting_mesh_assets.end());
    analysis.conflicting_mesh_assets.erase(std::unique(
        analysis.conflicting_mesh_assets.begin(), analysis.conflicting_mesh_assets.end()),
        analysis.conflicting_mesh_assets.end());
    analysis.read_set_digest = rebase_set_digest(
        "read", analysis.read_objects, analysis.read_mesh_assets);
    analysis.write_set_digest = rebase_set_digest(
        "write", analysis.write_objects, analysis.write_mesh_assets);
    return core::Result<AgentRebaseAnalysis>::success(std::move(analysis));
}

core::Result<AgentTask> AgentTask::rebase_checkpoint(
    application::ApplicationSession& source,
    const AgentTaskCheckpoint& checkpoint,
    AgentPlan reauthorized_plan) {
    const auto analysis = analyze_checkpoint_rebase(source, checkpoint);
    if (!analysis) return core::Result<AgentTask>::failure(analysis.error());
    if (!analysis.value().can_rebase()) {
        return core::Result<AgentTask>::failure(stale(
            "checkpoint rebase is blocked: " + analysis.value().blockers.front()));
    }
    if (analysis.value().current_revision == checkpoint.source_revision &&
        analysis.value().current_source_digest == checkpoint.source_digest) {
        return resume_checkpoint(source, checkpoint, std::move(reauthorized_plan));
    }
    if (reauthorized_plan.objective.base_revision != analysis.value().current_revision) {
        return core::Result<AgentTask>::failure(stale(
            "rebase plan must be bound to the exact current source revision"));
    }
    auto normalized_plan = reauthorized_plan;
    normalized_plan.objective.base_revision = checkpoint.source_revision;
    if (clarification_plan_context_digest(normalized_plan) != checkpoint.plan_context_digest) {
        return core::Result<AgentTask>::failure(stale(
            "rebase plan changed beyond its source revision; replan and review before rebasing"));
    }
    if (auto valid = reauthorized_plan.validate(); !valid) {
        return core::Result<AgentTask>::failure(valid.error());
    }

    auto saved_speculative = project::ProjectDocument::deserialize(
        checkpoint.speculative_project);
    if (!saved_speculative) return core::Result<AgentTask>::failure(saved_speculative.error());
    auto rebased = begin(source, std::move(reauthorized_plan), checkpoint.envelope);
    if (!rebased) return rebased;
    rebased.value().repairs_ = checkpoint.repair_lineage;
    rebased.value().rebases_ = checkpoint.rebase_lineage;
    rebased.value().ghost_corrections_ = checkpoint.ghost_corrections;
    rebased.value().replay_ghost_corrections_ =
        !rebased.value().ghost_corrections_.empty();
    AgentRebaseReceipt receipt;
    receipt.checkpoint_task_id = checkpoint.plan.id;
    receipt.checkpoint_plan_context_digest = checkpoint.plan_context_digest;
    receipt.previous_source_revision = checkpoint.source_revision;
    receipt.rebased_source_revision = analysis.value().current_revision;
    receipt.checkpoint_source_digest = checkpoint.source_digest;
    receipt.rebased_source_digest = analysis.value().current_source_digest;
    receipt.read_set_digest = analysis.value().read_set_digest;
    receipt.write_set_digest = analysis.value().write_set_digest;
    receipt.read_objects = analysis.value().read_objects;
    receipt.write_objects = analysis.value().write_objects;
    receipt.read_mesh_assets = analysis.value().read_mesh_assets;
    receipt.write_mesh_assets = analysis.value().write_mesh_assets;
    if (auto valid = receipt.validate(); !valid) {
        return core::Result<AgentTask>::failure(valid.error());
    }
    rebased.value().rebases_.push_back(std::move(receipt));
    const auto replayed = rebased.value().run();
    if (!replayed) return core::Result<AgentTask>::failure(replayed.error());
    rebased.value().cached_snapshot_.reset();

    if (checkpoint.status != TaskStatus::prepared) {
        bool write_result_matches = true;
        for (const scene::ObjectId object : analysis.value().write_objects) {
            if (!same_scene_object(
                    saved_speculative.value().scene().find(object),
                    rebased.value().speculative_->document_.scene().find(object))) {
                write_result_matches = false;
                break;
            }
        }
        for (const std::uint64_t asset : analysis.value().write_mesh_assets) {
            const auto old_mesh = saved_speculative.value().meshes().find(asset);
            const auto new_mesh = rebased.value().speculative_->document_.meshes().find(asset);
            const auto* old_value = old_mesh == saved_speculative.value().meshes().end()
                ? nullptr : &old_mesh->second;
            const auto* new_value = new_mesh ==
                    rebased.value().speculative_->document_.meshes().end()
                ? nullptr : &new_mesh->second;
            if (!same_mesh_state(old_value, new_value)) {
                write_result_matches = false;
                break;
            }
        }
        if (!write_result_matches) {
            rebased.value().status_ = TaskStatus::blocked;
            rebased.value().diagnostic_ = stale(
                "checkpoint rebase replay changed a task write-set result; acceptance is blocked");
            rebased.value().gates_ = rebased.value().evaluate_gates();
            rebased.value().cached_snapshot_.reset();
        }
    }
    return rebased;
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

const char* step_status_name(StepStatus status) noexcept {
    switch (status) {
    case StepStatus::pending: return "pending";
    case StepStatus::ready: return "ready";
    case StepStatus::running: return "running";
    case StepStatus::passed: return "passed";
    case StepStatus::failed: return "failed";
    case StepStatus::invalidated: return "invalidated";
    }
    return "unknown";
}

} // namespace carto::agent

namespace carto::application {

core::Result<DispatchReceipt> HumanApplicationAccess::accept_agent_task(
    ::carto::agent::AgentTask& task) {
    return task.accept(HumanActionAdmission{});
}

core::Result<void> HumanApplicationAccess::correct_agent_ghost_transform(
    ::carto::agent::AgentTask& task,
    scene::ObjectId object,
    core::Transform transform,
    std::string justification) {
    return task.correct_ghost_transform(object, transform, std::move(justification));
}

core::Result<void> HumanApplicationAccess::answer_agent_clarification(
    ::carto::agent::AgentPlan& plan,
    std::string_view question_id,
    std::optional<std::string> candidate_id,
    std::optional<std::string> freeform_answer) {
    if (candidate_id.has_value() == freeform_answer.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "clarification answer must select one candidate or provide one freeform answer"));
    }
    if (auto result = plan.validate(); !result) {
        return result;
    }
    auto& objective = plan.objective;
    const auto question = std::find_if(
        objective.clarifications.begin(), objective.clarifications.end(),
        [question_id](const auto& value) { return value.id == question_id; });
    if (question == objective.clarifications.end() ||
        question->selected_candidate_id.has_value() || question->freeform_answer.has_value() ||
        question->human_receipt.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "clarification is missing or already has a human answer"));
    }
    const auto context_digest = objective.clarification_context_digest(question_id);
    if (!context_digest) return core::Result<void>::failure(context_digest.error());
    const auto plan_context_digest = ::carto::agent::clarification_plan_context_digest(plan);
    if (candidate_id.has_value()) question->selected_candidate_id = std::move(candidate_id);
    else question->freeform_answer = std::move(freeform_answer);
    const auto answer_digest = objective.clarification_answer_digest(question_id);
    if (!answer_digest) {
        question->selected_candidate_id.reset();
        question->freeform_answer.reset();
        return core::Result<void>::failure(answer_digest.error());
    }
    question->human_receipt.emplace(::carto::agent::HumanClarificationReceipt(
        question->id, context_digest.value(), answer_digest.value(), plan_context_digest));
    if (auto result = plan.validate(); !result) {
        question->selected_candidate_id.reset();
        question->freeform_answer.reset();
        question->human_receipt.reset();
        return result;
    }
    return core::Result<void>::success();
}

} // namespace carto::application
