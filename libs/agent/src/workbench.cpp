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
constexpr double kMaxNearestObjectRadiusWorldUnits = 1'000'000.0;
constexpr std::uint32_t kKnownEffects = ::carto::agent::effect_mask(Effect::geometry) |
    ::carto::agent::effect_mask(Effect::topology) |
    ::carto::agent::effect_mask(Effect::transform) |
    ::carto::agent::effect_mask(Effect::material) |
    ::carto::agent::effect_mask(Effect::uv);

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

bool same_position(core::Vec3d left, core::Vec3d right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

core::Result<AgentDemonstrationParameters> parameterize_demo_step(
    const ai::Proposal& proposal,
    const project::ProjectDocument& document,
    const TargetRef& target) {
    AgentDemonstrationParameters parameters;
    if (proposal.preview_kind == editor::PreviewKind::poke_face) {
        if (proposal.tool_id != "mesh.poke-face" ||
            !std::holds_alternative<geometry::FaceId>(target.component) ||
            proposal.parameters.transform.has_value() ||
            proposal.parameters.distance.has_value() ||
            proposal.parameters.position.has_value()) {
            return core::Result<AgentDemonstrationParameters>::failure(invalid_state(
                "face-poke demonstration must bind one face and carry no parameters"));
        }
        parameters.parameterization =
            AgentDemonstrationParameterization::parameterless;
    } else if (proposal.preview_kind == editor::PreviewKind::inset_face ||
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

bool object_has_changes(const ObjectChange& change) noexcept {
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

bool same_attribute_value(
    const attributes::AttributeValue& left,
    const attributes::AttributeValue& right) {
    return std::visit([&right](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        const auto* other = std::get_if<Value>(&right);
        if (other == nullptr) return false;
        if constexpr (std::is_same_v<Value, core::Vec2d>) {
            return value.x == other->x && value.y == other->y;
        } else if constexpr (std::is_same_v<Value, core::Vec3d>) {
            return value.x == other->x && value.y == other->y && value.z == other->z;
        } else if constexpr (std::is_same_v<Value, core::Vec4d>) {
            return value.x == other->x && value.y == other->y &&
                value.z == other->z && value.w == other->w;
        } else {
            return value == *other;
        }
    }, left);
}

bool same_uv_set_descriptor(
    const attributes::UvSetDescriptor& left,
    const attributes::UvSetDescriptor& right) noexcept {
    return left.id == right.id && left.name == right.name &&
        left.layer_id == right.layer_id &&
        left.active_for_editing == right.active_for_editing &&
        left.active_for_render == right.active_for_render &&
        left.lightmap_candidate == right.lightmap_candidate &&
        left.tile_policy_version == right.tile_policy_version;
}

std::optional<std::map<geometry::CornerId, core::Vec2d>> uv_values_by_corner(
    const geometry::EditableMesh& mesh,
    std::string_view layer_id) {
    const auto* layer = mesh.attributes().find(layer_id);
    if (layer == nullptr ||
        layer->descriptor.domain != attributes::AttributeDomain::corner ||
        layer->descriptor.type != attributes::AttributeType::vec2) {
        return std::nullopt;
    }
    const auto topology = mesh.topology();
    if (!topology || topology.value().corners.size() != layer->values.size()) {
        return std::nullopt;
    }
    std::map<geometry::CornerId, core::Vec2d> values;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
        if (uv == nullptr || !uv->finite() ||
            !values.emplace(topology.value().corners[index].id, *uv).second) {
            return std::nullopt;
        }
    }
    return values;
}

bool same_vec3d(const core::Vec3d& left, const core::Vec3d& right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool same_vec4d(const core::Vec4d& left, const core::Vec4d& right) noexcept {
    return left.x == right.x && left.y == right.y &&
        left.z == right.z && left.w == right.w;
}

bool same_material_texture_dependencies(
    const render::StandardMaterial& left,
    const render::StandardMaterial& right) noexcept {
    return left.base_color == right.base_color &&
        left.metallic_roughness == right.metallic_roughness &&
        left.normal == right.normal && left.occlusion == right.occlusion &&
        left.emissive == right.emissive;
}

bool material_has_texture_dependencies(const render::StandardMaterial& material) noexcept {
    return static_cast<bool>(material.base_color) ||
        static_cast<bool>(material.metallic_roughness) ||
        static_cast<bool>(material.normal) || static_cast<bool>(material.occlusion) ||
        static_cast<bool>(material.emissive);
}

bool material_uses_texture(
    const render::StandardMaterial& material,
    assets::TextureAssetId texture) noexcept {
    return material.base_color.asset == texture ||
        material.metallic_roughness.asset == texture ||
        material.normal.asset == texture || material.occlusion.asset == texture ||
        material.emissive.asset == texture;
}

bool same_material_surface_definition(
    const render::StandardMaterial& left,
    const render::StandardMaterial& right) noexcept {
    return same_vec4d(left.base_color_factor, right.base_color_factor) &&
        left.metallic == right.metallic && left.roughness == right.roughness &&
        left.normal_scale == right.normal_scale &&
        left.occlusion_strength == right.occlusion_strength &&
        same_vec3d(left.emissive_factor, right.emissive_factor) &&
        left.emissive_strength == right.emissive_strength &&
        left.dielectric_f0 == right.dielectric_f0 &&
        left.clearcoat == right.clearcoat &&
        left.clearcoat_roughness == right.clearcoat_roughness &&
        left.alpha_mode == right.alpha_mode && left.alpha_cutoff == right.alpha_cutoff;
}

bool same_standard_material(
    const render::StandardMaterial& left,
    const render::StandardMaterial& right) noexcept {
    return same_material_surface_definition(left, right) &&
        same_material_texture_dependencies(left, right);
}

bool same_material_source(
    const render::MaterialAsset& left,
    const render::MaterialAsset& right) noexcept {
    return left.id == right.id && left.name == right.name &&
        left.revision == right.revision &&
        same_standard_material(left.definition, right.definition);
}

bool same_texture_source_content(
    const project::TextureSourceRecord& left,
    const project::TextureSourceRecord& right) noexcept {
    return left.content_digest == right.content_digest && left.format == right.format &&
        left.width == right.width && left.height == right.height;
}

bool same_texture_source_metadata(
    const project::TextureSourceRecord& left,
    const project::TextureSourceRecord& right) noexcept {
    return left.name == right.name && left.relative_locator == right.relative_locator &&
        left.provenance == right.provenance && left.revision == right.revision;
}

bool same_texture_source(
    const project::TextureSourceRecord& left,
    const project::TextureSourceRecord& right) noexcept {
    return left.id == right.id && same_texture_source_content(left, right) &&
        same_texture_source_metadata(left, right);
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
    const std::vector<std::uint64_t>& mesh_assets,
    const std::vector<project::MaterialRegionKey>& material_regions) {
    std::string canonical = "cartographer.agent.rebase-set.v1|";
    append_field(canonical, label);
    append_number(canonical, objects.size());
    for (const auto object : objects) append_number(canonical, object.value);
    append_number(canonical, mesh_assets.size());
    for (const auto mesh_asset : mesh_assets) append_number(canonical, mesh_asset);
    if (!material_regions.empty()) {
        append_field(canonical, "material-regions");
        append_number(canonical, material_regions.size());
        for (const auto key : material_regions) {
            append_number(canonical, key.mesh_asset);
            append_number(canonical, key.region_id);
        }
    }
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
    if ((effects & ::carto::agent::effect_mask(Effect::material)) != 0U) append("material");
    if ((effects & ::carto::agent::effect_mask(Effect::uv)) != 0U) append("uv");
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
        const bool decimal_point = text[index] == '.' && index + 1U < text.size() &&
            is_digit(static_cast<unsigned char>(text[index + 1U])) &&
            (index == 0U || is_digit(static_cast<unsigned char>(text[index - 1U])) ||
                (!is_ascii_alphanumeric(static_cast<unsigned char>(text[index - 1U])) &&
                    text[index - 1U] != '-'));
        const bool negative_number_sign = text[index] == '-' && index + 1U < text.size() &&
            (is_digit(static_cast<unsigned char>(text[index + 1U])) ||
                (text[index + 1U] == '.' && index + 2U < text.size() &&
                    is_digit(static_cast<unsigned char>(text[index + 2U])))) &&
            (index == 0U || !is_ascii_alphanumeric(
                static_cast<unsigned char>(text[index - 1U])));
        if (is_ascii_alphanumeric(character) || decimal_point || negative_number_sign) {
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

bool intent_starts_with(
    std::string_view intent,
    std::string_view first,
    std::string_view second,
    std::optional<std::string_view> third = std::nullopt) {
    const auto tokens = objective_tokens(intent);
    return tokens.size() >= (third.has_value() ? 3U : 2U) &&
        tokens[0] == first && tokens[1] == second &&
        (!third.has_value() || tokens[2] == *third);
}

struct MaterialAssignmentIntent {
    std::uint64_t material = 0U;
    std::uint32_t region = 0U;
    std::optional<std::uint64_t> variant;
};

std::optional<MaterialAssignmentIntent> parse_material_assignment_intent(
    std::string_view intent) {
    const auto tokens = objective_tokens(intent);
    const bool with_variant = tokens.size() == 8U && tokens[6] == "variant";
    if (!(tokens.size() == 6U || with_variant) || tokens[0] != "assign" ||
        tokens[1] != "material" || tokens[3] != "to" || tokens[4] != "region") {
        return std::nullopt;
    }
    const auto parse_identity = [](const std::string& value)
            -> std::optional<std::uint64_t> {
        std::uint64_t parsed_value = 0U;
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), parsed_value);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            parsed_value == 0U) {
            return std::nullopt;
        }
        return parsed_value;
    };
    const auto material = parse_identity(tokens[2]);
    const auto region = parse_identity(tokens[5]);
    if (!material.has_value() || !region.has_value() ||
        *region > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    MaterialAssignmentIntent parsed{*material, static_cast<std::uint32_t>(*region),
        std::nullopt};
    if (with_variant) {
        parsed.variant = parse_identity(tokens[7]);
        if (!parsed.variant.has_value()) return std::nullopt;
    }
    return parsed;
}

core::Result<std::string> material_catalog_source_projection(
    const project::MaterialCatalog& catalog) {
    auto without_assignments = catalog;
    std::vector<project::MaterialRegionKey> keys;
    keys.reserve(without_assignments.assignments().size());
    for (const auto& [key, assignment] : without_assignments.assignments()) {
        static_cast<void>(assignment);
        keys.push_back(key);
    }
    for (const auto key : keys) {
        if (auto erased = without_assignments.erase_assignment(key); !erased) {
            return core::Result<std::string>::failure(erased.error());
        }
    }
    return core::Result<std::string>::success(without_assignments.serialize());
}

bool same_material_assignment_at(
    const project::MaterialCatalog& left,
    const project::MaterialCatalog& right,
    project::MaterialRegionKey key) {
    const auto left_assignment = left.assignments().find(key);
    const auto right_assignment = right.assignments().find(key);
    if (left_assignment == left.assignments().end() ||
        right_assignment == right.assignments().end()) {
        return left_assignment == left.assignments().end() &&
            right_assignment == right.assignments().end();
    }
    return left_assignment->second.key == right_assignment->second.key &&
        left_assignment->second.material == right_assignment->second.material &&
        left_assignment->second.variant == right_assignment->second.variant;
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

bool matches_objective_phrase_at(
    const std::vector<std::string>& tokens,
    std::size_t start,
    std::span<const std::string_view> phrase) {
    if (phrase.empty() || start > tokens.size() || phrase.size() > tokens.size() - start) {
        return false;
    }
    for (std::size_t offset = 0U; offset < phrase.size(); ++offset) {
        if (tokens[start + offset] != phrase[offset]) return false;
    }
    return true;
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
            token == "keep" || token == "maximum" || token == "limit" ||
            token == "displacement" || token == "unless" || token == "except" ||
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

core::Result<std::optional<double>> compile_vertex_displacement_bound(
    const std::vector<std::string>& tokens,
    std::vector<bool>& consumed) {
    constexpr std::array<std::array<std::string_view, 4U>, 3U> prefixes{{
        {"maximum", "vertex", "displacement", {}},
        {"limit", "vertex", "displacement", "to"},
        {"vertex", "displacement", "at", "most"},
    }};
    constexpr std::array<std::size_t, 3U> prefix_sizes{3U, 4U, 4U};
    std::optional<double> bound;
    for (std::size_t start = 0U; start < tokens.size(); ++start) {
        for (std::size_t prefix_index = 0U; prefix_index < prefixes.size(); ++prefix_index) {
            const auto phrase = std::span<const std::string_view>(
                prefixes[prefix_index].data(), prefix_sizes[prefix_index]);
            if (!matches_objective_phrase_at(tokens, start, phrase)) continue;
            if (bound.has_value()) {
                return core::Result<std::optional<double>>::failure(invalid(
                    "objective may declare only one vertex-displacement bound"));
            }
            const std::size_t value_index = start + prefix_sizes[prefix_index];
            if (value_index + 2U >= tokens.size() || tokens[value_index + 1U] != "world" ||
                tokens[value_index + 2U] != "units") {
                return core::Result<std::optional<double>>::failure(invalid(
                    "vertex-displacement bound must include a number followed by 'world units'"));
            }
            const auto& value_text = tokens[value_index];
            double value = 0.0;
            const auto parsed = std::from_chars(
                value_text.data(), value_text.data() + value_text.size(), value,
                std::chars_format::general);
            if (parsed.ec != std::errc{} || parsed.ptr != value_text.data() + value_text.size() ||
                !std::isfinite(value) || value <= 0.0 || value > 1'000'000.0) {
                return core::Result<std::optional<double>>::failure(invalid(
                    "vertex-displacement bound must be finite, positive, and at most 1000000 world units"));
            }
            bound = value;
            for (std::size_t index = start; index < value_index + 3U; ++index) {
                consumed[index] = true;
            }
        }
    }
    return core::Result<std::optional<double>>::success(bound);
}

core::Result<void> save_immutable_artifact(
    const std::filesystem::path& path,
    std::string_view bytes,
    std::string_view label) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            std::string(label) + " destination must name a file"));
    }
    std::error_code error;
    const auto destination_status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect " + std::string(label) + " destination"));
    }
    if (std::filesystem::exists(destination_status)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            std::string(label) + " artifacts are immutable; choose a new destination path"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        const auto parent_status = std::filesystem::status(parent, error);
        if (error || !std::filesystem::is_directory(parent_status)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                std::string(label) + " destination directory does not exist"));
        }
    }

    auto lock_path = path;
    lock_path += ".lock";
    if (!std::filesystem::create_directory(lock_path, error)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            std::string(label) + " destination is already being written"));
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
                core::ErrorCode::io_error,
                "unable to create " + std::string(label) + " temporary file"));
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary_path, error);
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to flush " + std::string(label) + " temporary file"));
        }
    }
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to publish immutable " + std::string(label)));
    }
    return core::Result<void>::success();
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
    case AgentDemonstrationParameterization::parameterless:
        if (tool_id != "mesh.poke-face" || normalized_distance != 0.0 ||
            !displacement_is_zero) {
            return core::Result<void>::failure(invalid(
                "parameterless demonstration is invalid for its native tool"));
        }
        return core::Result<void>::success();
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
        {
            "lightmap-uv-generation",
            "Generate unique lightmap UVs",
            "Adds a deterministic, revision-bound lightmap-candidate UV set without changing mesh topology or existing UV sets.",
            0U,
            ::carto::agent::effect_mask(Effect::uv),
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

core::Result<ParsedAgentGoal> parse_agent_goal(std::string_view goal) {
    if (!safe_text(goal, kMaxGoalBytes)) {
        return core::Result<ParsedAgentGoal>::failure(invalid(
            "objective goal is empty, malformed, or exceeds its text bound"));
    }

    std::size_t prefix_begin = 0U;
    while (prefix_begin < goal.size() && goal[prefix_begin] == ' ') ++prefix_begin;
    constexpr std::string_view kNamedScopePrefix = "on object";
    constexpr std::string_view kNearestScopePrefix = "on nearest object within";
    const auto ascii_lower = [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte >= 'A' && byte <= 'Z'
            ? static_cast<char>(byte + ('a' - 'A'))
            : static_cast<char>(byte);
    };
    const auto prefix_matches = [&](std::string_view prefix) {
        if (goal.size() - prefix_begin < prefix.size()) return false;
        for (std::size_t index = 0U; index < prefix.size(); ++index) {
            if (ascii_lower(goal[prefix_begin + index]) != prefix[index]) return false;
        }
        return true;
    };
    const auto phrase_matches = [&](std::size_t offset, std::string_view phrase) {
        if (goal.size() - offset < phrase.size()) return false;
        for (std::size_t index = 0U; index < phrase.size(); ++index) {
            if (ascii_lower(goal[offset + index]) != phrase[index]) return false;
        }
        return true;
    };
    const bool nearest_scope = prefix_matches(kNearestScopePrefix) &&
        (goal.size() == prefix_begin + kNearestScopePrefix.size() ||
         goal[prefix_begin + kNearestScopePrefix.size()] == ' ');
    const bool named_scope = prefix_matches(kNamedScopePrefix) &&
        (goal.size() == prefix_begin + kNamedScopePrefix.size() ||
         goal[prefix_begin + kNamedScopePrefix.size()] == ' ');
    if (!nearest_scope && !named_scope) {
        ParsedAgentGoal parsed;
        parsed.commands = std::string(goal);
        return core::Result<ParsedAgentGoal>::success(std::move(parsed));
    }

    std::size_t cursor = prefix_begin +
        (nearest_scope ? kNearestScopePrefix.size() : kNamedScopePrefix.size());
    std::optional<double> radius_world_units;
    if (nearest_scope) {
        while (cursor < goal.size() && goal[cursor] == ' ') ++cursor;
        double radius = 0.0;
        const auto parsed_radius = std::from_chars(
            goal.data() + cursor, goal.data() + goal.size(), radius,
            std::chars_format::general);
        if (parsed_radius.ec != std::errc{} || parsed_radius.ptr == goal.data() + cursor ||
            !std::isfinite(radius) || radius <= 0.0 ||
            radius > kMaxNearestObjectRadiusWorldUnits) {
            return core::Result<ParsedAgentGoal>::failure(invalid(
                "nearest-object scope radius must be positive and at most 1000000 world units"));
        }
        radius_world_units = radius;
        cursor = static_cast<std::size_t>(parsed_radius.ptr - goal.data());
        while (cursor < goal.size() && goal[cursor] == ' ') ++cursor;
        constexpr std::string_view kRadiusSuffix = "world units of";
        if (!phrase_matches(cursor, kRadiusSuffix)) {
            return core::Result<ParsedAgentGoal>::failure(invalid(
                "nearest-object scope must use '<radius> world units of \"Anchor Name\"'"));
        }
        cursor += kRadiusSuffix.size();
        while (cursor < goal.size() && goal[cursor] == ' ') ++cursor;
    } else {
        while (cursor < goal.size() && goal[cursor] == ' ') ++cursor;
    }
    if (cursor >= goal.size() || goal[cursor] != '"') {
        return core::Result<ParsedAgentGoal>::failure(invalid(
            nearest_scope
                ? "nearest-object scope must name its exact anchor in double quotes"
                : "named goal target must use on object \"Exact Name\": command syntax"));
    }
    ++cursor;

    std::string target_name;
    bool closed = false;
    while (cursor < goal.size()) {
        char value = goal[cursor++];
        if (value == '"') {
            closed = true;
            break;
        }
        if (value == '\\') {
            if (cursor >= goal.size()) {
                return core::Result<ParsedAgentGoal>::failure(invalid(
                    "named goal target ends with an incomplete escape"));
            }
            value = goal[cursor++];
            if (value != '"' && value != '\\') {
                return core::Result<ParsedAgentGoal>::failure(invalid(
                    "named goal target supports only escaped quotes and backslashes"));
            }
        }
        target_name.push_back(value);
        if (target_name.size() > 256U) {
            return core::Result<ParsedAgentGoal>::failure(invalid(
                "named goal target name exceeds the 256-byte stable-name limit"));
        }
    }
    if (!closed || target_name.empty()) {
        return core::Result<ParsedAgentGoal>::failure(invalid(
            "named goal target must contain a closed, non-empty exact name"));
    }
    while (cursor < goal.size() && goal[cursor] == ' ') ++cursor;
    if (cursor >= goal.size() || goal[cursor] != ':') {
        return core::Result<ParsedAgentGoal>::failure(invalid(
            "named goal target must end with a colon before its command"));
    }
    ++cursor;
    while (cursor < goal.size() && goal[cursor] == ' ') ++cursor;
    if (cursor == goal.size()) {
        return core::Result<ParsedAgentGoal>::failure(invalid(
            "scoped goal target must include a native command after the colon"));
    }
    ParsedAgentGoal parsed;
    parsed.commands = std::string(goal.substr(cursor));
    if (nearest_scope) {
        parsed.nearest_object_scope.emplace(ParsedAgentGoal::NearestObjectScope{
            std::move(target_name), *radius_world_units});
    } else {
        parsed.exact_object_name = std::move(target_name);
    }
    return core::Result<ParsedAgentGoal>::success(std::move(parsed));
}

core::Result<std::vector<HardConstraint>> compile_explicit_hard_constraints(
    std::string_view goal) {
    const auto parsed_goal = parse_agent_goal(goal);
    if (!parsed_goal) {
        return core::Result<std::vector<HardConstraint>>::failure(parsed_goal.error());
    }
    const auto tokens = objective_tokens(parsed_goal.value().commands);
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
    const bool preserves_transform =
        contains_objective_phrase(tokens, {"preserve", "transform"}) ||
        contains_objective_phrase(tokens, {"preserve", "transforms"}) ||
        contains_objective_phrase(tokens, {"preserve", "world", "transform"}) ||
        contains_objective_phrase(tokens, {"preserve", "world", "transforms"}) ||
        contains_objective_phrase(tokens, {"preserve", "object", "transform"}) ||
        contains_objective_phrase(tokens, {"preserve", "object", "transforms"}) ||
        contains_objective_phrase(tokens, {"keep", "transform", "unchanged"}) ||
        contains_objective_phrase(tokens, {"keep", "transforms", "unchanged"}) ||
        contains_objective_phrase(tokens, {"keep", "world", "transform", "unchanged"}) ||
        contains_objective_phrase(tokens, {"keep", "world", "transforms", "unchanged"}) ||
        contains_objective_phrase(tokens, {"keep", "object", "transform", "unchanged"}) ||
        contains_objective_phrase(tokens, {"keep", "object", "transforms", "unchanged"}) ||
        contains_objective_phrase(tokens, {"do", "not", "change", "transform"}) ||
        contains_objective_phrase(tokens, {"do", "not", "change", "transforms"}) ||
        contains_objective_phrase(tokens, {"do", "not", "change", "world", "transform"}) ||
        contains_objective_phrase(tokens, {"do", "not", "change", "world", "transforms"}) ||
        contains_objective_phrase(tokens, {"do", "not", "change", "object", "transform"}) ||
        contains_objective_phrase(tokens, {"do", "not", "change", "object", "transforms"}) ||
        contains_objective_phrase(tokens, {"don", "t", "change", "transform"}) ||
        contains_objective_phrase(tokens, {"don", "t", "change", "transforms"}) ||
        contains_objective_phrase(tokens, {"transforms", "must", "remain", "unchanged"}) ||
        contains_objective_phrase(tokens, {"transform", "must", "remain", "unchanged"}) ||
        contains_objective_phrase(tokens, {"world", "transforms", "must", "remain", "unchanged"});
    const bool permits_transform_change =
        contains_objective_phrase(tokens, {"do", "not", "preserve", "transform"}) ||
        contains_objective_phrase(tokens, {"do", "not", "preserve", "transforms"}) ||
        contains_objective_phrase(tokens, {"don", "t", "preserve", "transform"}) ||
        contains_objective_phrase(tokens, {"don", "t", "preserve", "transforms"}) ||
        contains_objective_phrase(tokens, {"allow", "transform", "changes"}) ||
        contains_objective_phrase(tokens, {"allow", "transforms", "to", "change"}) ||
        contains_objective_phrase(tokens, {"permit", "transform", "changes"}) ||
        contains_objective_phrase(tokens, {"permit", "transforms", "to", "change"}) ||
        contains_objective_phrase(tokens, {"transforms", "may", "change"}) ||
        contains_objective_phrase(tokens, {"transform", "may", "change"}) ||
        contains_objective_phrase(tokens, {"world", "transforms", "may", "change"});
    if (preserves_topology && permits_topology_change) {
        return core::Result<std::vector<HardConstraint>>::failure(invalid(
            "objective gives conflicting topology requirements"));
    }
    if (preserves_transform && permits_transform_change) {
        return core::Result<std::vector<HardConstraint>>::failure(invalid(
            "objective gives conflicting transform requirements"));
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

    consume_objective_phrase(tokens, consumed, {"preserve", "transform"});
    consume_objective_phrase(tokens, consumed, {"preserve", "transforms"});
    consume_objective_phrase(tokens, consumed, {"preserve", "world", "transform"});
    consume_objective_phrase(tokens, consumed, {"preserve", "world", "transforms"});
    consume_objective_phrase(tokens, consumed, {"preserve", "object", "transform"});
    consume_objective_phrase(tokens, consumed, {"preserve", "object", "transforms"});
    consume_objective_phrase(tokens, consumed, {"keep", "transform", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"keep", "transforms", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"keep", "world", "transform", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"keep", "world", "transforms", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"keep", "object", "transform", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"keep", "object", "transforms", "unchanged"});
    consume_objective_phrase(tokens, consumed, {"do", "not", "change", "transform"});
    consume_objective_phrase(tokens, consumed, {"do", "not", "change", "transforms"});
    consume_objective_phrase(tokens, consumed,
        {"do", "not", "change", "world", "transform"});
    consume_objective_phrase(tokens, consumed,
        {"do", "not", "change", "world", "transforms"});
    consume_objective_phrase(tokens, consumed,
        {"do", "not", "change", "object", "transform"});
    consume_objective_phrase(tokens, consumed,
        {"do", "not", "change", "object", "transforms"});
    consume_objective_phrase(tokens, consumed, {"don", "t", "change", "transform"});
    consume_objective_phrase(tokens, consumed, {"don", "t", "change", "transforms"});
    consume_objective_phrase(tokens, consumed,
        {"transforms", "must", "remain", "unchanged"});
    consume_objective_phrase(tokens, consumed,
        {"transform", "must", "remain", "unchanged"});
    consume_objective_phrase(tokens, consumed,
        {"world", "transforms", "must", "remain", "unchanged"});
    consume_objective_phrase(tokens, consumed,
        {"do", "not", "preserve", "transform"});
    consume_objective_phrase(tokens, consumed,
        {"do", "not", "preserve", "transforms"});
    consume_objective_phrase(tokens, consumed,
        {"don", "t", "preserve", "transform"});
    consume_objective_phrase(tokens, consumed,
        {"don", "t", "preserve", "transforms"});
    consume_objective_phrase(tokens, consumed, {"allow", "transform", "changes"});
    consume_objective_phrase(tokens, consumed, {"allow", "transforms", "to", "change"});
    consume_objective_phrase(tokens, consumed, {"permit", "transform", "changes"});
    consume_objective_phrase(tokens, consumed, {"permit", "transforms", "to", "change"});
    consume_objective_phrase(tokens, consumed, {"transforms", "may", "change"});
    consume_objective_phrase(tokens, consumed, {"transform", "may", "change"});
    consume_objective_phrase(tokens, consumed, {"world", "transforms", "may", "change"});

    auto displacement_bound = compile_vertex_displacement_bound(tokens, consumed);
    if (!displacement_bound) {
        return core::Result<std::vector<HardConstraint>>::failure(
            displacement_bound.error());
    }

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
    if (preserves_transform) {
        constraints.push_back(HardConstraint{
            "compiled.preserve-transform",
            ObjectiveConstraintKind::preserve_transform,
            std::nullopt,
            0.0,
            "Compiled from explicit goal language requiring unchanged world transforms."});
    }
    if (displacement_bound.value().has_value()) {
        constraints.push_back(HardConstraint{
            "compiled.maximum-vertex-displacement",
            ObjectiveConstraintKind::maximum_vertex_displacement,
            std::nullopt,
            *displacement_bound.value(),
            "Compiled from an explicit maximum world-space vertex-displacement bound."});
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
    if (!safe_text(task_id, kMaxTaskIdBytes) || !object || source_revision.exhausted() ||
        branch_revision_before.exhausted() ||
        branch_revision_after != branch_revision_before.next() || plan_context_digest.is_zero() ||
        !safe_text(justification, 1024U) ||
        (recompute_after_step_id.has_value() && !recompute_after_step_id->empty() &&
            !safe_text(*recompute_after_step_id, kMaxStepIdBytes))) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "ghost correction evidence is incomplete or outside its bounds"));
    }
    std::string canonical;
    switch (kind) {
    case GhostCorrectionKind::object_transform:
        if (vertex.has_value() || face.has_value() || !same_position(before_position, {}) ||
            !same_position(after_position, {}) || !before_transform.finite() ||
            !after_transform.finite() || same_transform(before_transform, after_transform) ||
            distance != 0.0 || !before_mesh_digest.is_zero() || !after_mesh_digest.is_zero()) {
            return core::Result<assets::Sha256Digest>::failure(invalid(
                "object-transform correction contains invalid or unrelated component evidence"));
        }
        // Preserve the v1 digest for legacy transform receipts in v5 checkpoints.
        canonical = "cartographer.agent.ghost-correction.v1|";
        break;
    case GhostCorrectionKind::vertex_position:
        if (!vertex.has_value() || !*vertex || face.has_value() || !before_position.finite() ||
            !after_position.finite() || same_position(before_position, after_position) ||
            !same_transform(before_transform, core::Transform::identity()) ||
            !same_transform(after_transform, core::Transform::identity()) || distance != 0.0 ||
            (before_mesh_digest.is_zero() != after_mesh_digest.is_zero())) {
            return core::Result<assets::Sha256Digest>::failure(invalid(
                "vertex-position correction contains invalid or unrelated transform evidence"));
        }
        canonical = before_mesh_digest.is_zero()
            ? "cartographer.agent.ghost-correction.v2|"
            : "cartographer.agent.ghost-correction.v3|";
        append_number(canonical, static_cast<std::uint64_t>(kind));
        break;
    case GhostCorrectionKind::face_inset:
    case GhostCorrectionKind::face_extrusion:
        if (vertex.has_value() || !face.has_value() || !*face ||
            !same_position(before_position, {}) || !same_position(after_position, {}) ||
            !same_transform(before_transform, core::Transform::identity()) ||
            !same_transform(after_transform, core::Transform::identity()) ||
            !std::isfinite(distance) || distance <= 0.0 || distance > 1'000'000.0 ||
            before_mesh_digest.is_zero() || after_mesh_digest.is_zero() ||
            before_mesh_digest == after_mesh_digest) {
            return core::Result<assets::Sha256Digest>::failure(invalid(
                "face-topology correction contains invalid component or mesh evidence"));
        }
        canonical = "cartographer.agent.ghost-correction.v3|";
        append_number(canonical, static_cast<std::uint64_t>(kind));
        break;
    default:
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "ghost correction kind is unknown"));
    }
    if (recompute_after_step_id.has_value()) {
        canonical = "cartographer.agent.ghost-correction.v4|";
        append_number(canonical, static_cast<std::uint64_t>(kind));
        append_number(canonical, 1U);
        append_field(canonical, *recompute_after_step_id);
    }
    append_field(canonical, task_id);
    append_number(canonical, object.value);
    if (kind == GhostCorrectionKind::vertex_position) {
        append_number(canonical, vertex->value);
        append_double(canonical, before_position.x);
        append_double(canonical, before_position.y);
        append_double(canonical, before_position.z);
        append_double(canonical, after_position.x);
        append_double(canonical, after_position.y);
        append_double(canonical, after_position.z);
        if (!before_mesh_digest.is_zero()) {
            append_field(canonical, before_mesh_digest.hex());
            append_field(canonical, after_mesh_digest.hex());
        }
    } else if (kind == GhostCorrectionKind::face_inset ||
        kind == GhostCorrectionKind::face_extrusion) {
        append_number(canonical, face->value);
        append_double(canonical, distance);
        append_field(canonical, before_mesh_digest.hex());
        append_field(canonical, after_mesh_digest.hex());
    }
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

core::Result<assets::Sha256Digest>
AgentGhostRecomputeReceipt::computed_evidence_digest() const {
    if (!safe_text(task_id, kMaxTaskIdBytes) ||
        (!anchor_step_id.empty() && !safe_text(anchor_step_id, kMaxStepIdBytes)) ||
        correction_evidence_digest.is_zero() || previous_diff_digest.is_zero() ||
        result_diff_digest.is_zero() || dependent_step_ids.size() > kMaxPlanSteps ||
        previous_step_receipts.size() > 4096U || previous_gate_results.size() > 256U ||
        (!replay_completed && !diagnostic.has_value()) ||
        (diagnostic.has_value() && diagnostic->message.size() > 2048U)) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "ghost recompute evidence is incomplete or outside its bounds"));
    }
    std::set<std::string> unique_steps;
    for (const auto& step_id : dependent_step_ids) {
        if (!safe_text(step_id, kMaxStepIdBytes) || step_id == anchor_step_id ||
            !unique_steps.insert(step_id).second) {
            return core::Result<assets::Sha256Digest>::failure(invalid(
                "ghost recompute dependent-step lineage is invalid"));
        }
    }

    std::string canonical = "cartographer.agent.ghost-recompute.v1|";
    append_field(canonical, task_id);
    append_field(canonical, anchor_step_id);
    append_field(canonical, correction_evidence_digest.hex());
    append_number(canonical, dependent_step_ids.size());
    for (const auto& step_id : dependent_step_ids) append_field(canonical, step_id);
    append_number(canonical, previous_step_receipts.size());
    for (const auto& receipt : previous_step_receipts) {
        append_field(canonical, receipt.step_id);
        append_field(canonical, receipt.intent);
        append_field(canonical, receipt.tool_id);
        append_number(canonical, receipt.target_object.value);
        append_number(canonical, receipt.revision_before.value());
        append_number(canonical, receipt.revision_after.value());
        append_number(canonical, static_cast<std::uint64_t>(receipt.status));
        append_number(canonical, receipt.diagnostic.has_value() ? 1U : 0U);
        if (receipt.diagnostic.has_value()) {
            append_number(canonical,
                static_cast<std::uint64_t>(receipt.diagnostic->code));
            append_field(canonical, receipt.diagnostic->message);
        }
    }
    append_number(canonical, previous_gate_results.size());
    for (const auto& gate : previous_gate_results) {
        append_field(canonical, gate.id);
        append_number(canonical, gate.required ? 1U : 0U);
        append_number(canonical, gate.passed ? 1U : 0U);
        append_field(canonical, gate.evidence);
        append_field(canonical, gate.validator_version);
        append_number(canonical, gate.source_revision.value());
        append_number(canonical, gate.evaluated_revision.value());
        append_number(canonical, gate.affected_objects.size());
        for (const auto object : gate.affected_objects) append_number(canonical, object.value);
        append_number(canonical, gate.affected_asset_ids.size());
        for (const auto asset : gate.affected_asset_ids) append_number(canonical, asset);
        append_field(canonical, gate.result_digest.hex());
        append_field(canonical, gate.evidence_digest.hex());
        append_number(canonical, gate.diagnostic.has_value() ? 1U : 0U);
        if (gate.diagnostic.has_value()) {
            append_number(canonical, static_cast<std::uint64_t>(gate.diagnostic->code));
            append_field(canonical, gate.diagnostic->message);
        }
    }
    append_field(canonical, previous_diff_digest.hex());
    append_field(canonical, result_diff_digest.hex());
    append_number(canonical, previous_speculative_revision.value());
    append_number(canonical, result_speculative_revision.value());
    append_number(canonical, replay_completed ? 1U : 0U);
    append_number(canonical, diagnostic.has_value() ? 1U : 0U);
    if (diagnostic.has_value()) {
        append_number(canonical, static_cast<std::uint64_t>(diagnostic->code));
        append_field(canonical, diagnostic->message);
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical));
}

core::Result<void> AgentGhostRecomputeReceipt::validate() const {
    const auto computed = computed_evidence_digest();
    if (!computed) return core::Result<void>::failure(computed.error());
    if (evidence_digest.is_zero() || evidence_digest != computed.value()) {
        return core::Result<void>::failure(invalid(
            "ghost recompute digest does not bind the prior and replayed branches"));
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
        if (maximum_displacement != 0.0) {
            return core::Result<void>::failure(invalid(
                "transform-preservation constraint cannot declare a numeric bound"));
        }
        break;
    case ObjectiveConstraintKind::maximum_vertex_displacement:
        if (!std::isfinite(maximum_displacement) ||
            maximum_displacement <= 0.0) {
            return core::Result<void>::failure(invalid(
                "vertex-displacement constraint requires a finite positive bound"));
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
        max_additional_operations > kMaxPlanSteps ||
        max_ghost_recomputations > 16U) {
        return core::Result<void>::failure(invalid(
            "work envelope repair or ghost-recompute budget exceeds its supported bound"));
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
        objects.begin(), objects.end(), object_has_changes));
}

std::size_t ProjectDiff::changed_mesh_count() const noexcept {
    return meshes.size();
}

std::size_t ProjectDiff::changed_material_binding_count() const noexcept {
    return material_bindings.size();
}

std::size_t ProjectDiff::changed_material_source_count() const noexcept {
    return material_sources.size();
}

std::size_t ProjectDiff::changed_texture_source_count() const noexcept {
    return texture_sources.size();
}

bool ProjectDiff::has_changes() const noexcept {
    return changed_object_count() != 0U || changed_mesh_count() != 0U ||
        changed_material_binding_count() != 0U || changed_material_source_count() != 0U ||
        changed_texture_source_count() != 0U || material_catalog_changed ||
        engineering_changed || condition_changed;
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
    const auto ordered_material_regions = [](
            const std::vector<project::MaterialRegionKey>& values) {
        return std::all_of(values.begin(), values.end(), [](project::MaterialRegionKey key) {
                return key.mesh_asset != 0U && key.region_id != 0U;
            }) && std::is_sorted(values.begin(), values.end()) &&
            std::adjacent_find(values.begin(), values.end()) == values.end();
    };
    const auto ordered_step_ids = [](const std::vector<std::string>& values) {
        return values.size() <= kMaxPlanSteps &&
            std::all_of(values.begin(), values.end(), [](const std::string& value) {
                return safe_text(value, kMaxTaskIdBytes);
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
        read_material_regions.size() > kMaxScopeObjects ||
        write_material_regions.size() > kMaxScopeObjects ||
        !ordered_objects(read_objects) || !ordered_objects(write_objects) ||
        !ordered_assets(read_mesh_assets) || !ordered_assets(write_mesh_assets) ||
        !ordered_material_regions(read_material_regions) ||
        !ordered_material_regions(write_material_regions) ||
        !ordered_step_ids(reused_step_ids) ||
        !subset(write_objects, read_objects) || !subset(write_mesh_assets, read_mesh_assets) ||
        !subset(write_material_regions, read_material_regions) ||
        std::any_of(read_material_regions.begin(), read_material_regions.end(),
            [this](project::MaterialRegionKey key) {
                return !std::binary_search(
                    read_mesh_assets.begin(), read_mesh_assets.end(), key.mesh_asset);
            }) ||
        std::any_of(write_material_regions.begin(), write_material_regions.end(),
            [this](project::MaterialRegionKey key) {
                return !std::binary_search(
                    write_mesh_assets.begin(), write_mesh_assets.end(), key.mesh_asset);
            }) ||
        rebase_set_digest("read", read_objects, read_mesh_assets,
            read_material_regions) != read_set_digest ||
        rebase_set_digest("write", write_objects, write_mesh_assets,
            write_material_regions) != write_set_digest) {
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
        envelope.max_ghost_recomputations >
            failed_attempt.envelope_.max_ghost_recomputations ||
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
    next_attempt.value().ghost_recompute_lineage_ =
        failed_attempt.ghost_recompute_lineage_;
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
    const auto anchor_step = ghost_correction_anchor(
        object, GhostCorrectionKind::object_transform);
    if (!anchor_step) return core::Result<void>::failure(anchor_step.error());
    const auto dependent_steps = anchor_step.value().empty()
        ? std::vector<std::string>{} : dependent_plan_steps(anchor_step.value());
    if (!dependent_steps.empty() &&
        ghost_recompute_lineage_.size() >= envelope_.max_ghost_recomputations) {
        return core::Result<void>::failure(policy(
            "work envelope ghost-recompute budget is exhausted"));
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
    correction.recompute_after_step_id = anchor_step.value();
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
    const auto correction_evidence_digest = correction.evidence_digest;
    ghost_corrections_.push_back(std::move(correction));
    gates_ = evaluate_gates();
    cached_snapshot_.reset();
    return recompute_after_ghost_correction(
        correction_evidence_digest, anchor_step.value(), dependent_steps);
}

core::Result<void> AgentTask::correct_ghost_vertex_position(
    scene::ObjectId object,
    geometry::VertexId vertex,
    core::Vec3d position,
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
    if (!position.finite() || !vertex) {
        return core::Result<void>::failure(invalid(
            "ghost vertex correction requires a valid vertex and finite position"));
    }
    if (ghost_corrections_.size() >= 256U) {
        return core::Result<void>::failure(policy(
            "ghost correction evidence budget is exhausted"));
    }
    const auto* target = speculative_->document_.scene().find(object);
    if (target == nullptr || target->locked || !target->mesh_asset.has_value()) {
        return core::Result<void>::failure(policy(
            "ghost vertex correction requires an unlocked mesh object"));
    }
    const auto mesh = speculative_->document_.meshes().find(*target->mesh_asset);
    if (mesh == speculative_->document_.meshes().end()) {
        return core::Result<void>::failure(stale(
            "ghost vertex correction target has no current mesh asset"));
    }
    const auto* current_vertex = mesh->second.find_vertex(vertex);
    if (current_vertex == nullptr) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "ghost correction vertex is absent from the speculative mesh"));
    }
    for (const auto& instance : speculative_->document_.scene().objects_sorted()) {
        if (instance.mesh_asset != target->mesh_asset) continue;
        if (!scope_contains(instance.id) ||
            !contains(plan_.objective.scope_objects, instance.id) || instance.locked ||
            contains(plan_.objective.protected_objects, instance.id)) {
            return core::Result<void>::failure(policy(
                "all instances of a corrected shared mesh must be in-scope, unlocked, and unprotected"));
        }
    }
    if (same_position(current_vertex->position, position)) {
        return core::Result<void>::failure(invalid(
            "ghost correction does not change the selected vertex"));
    }
    const auto plan_context_digest = plan_.context_digest();
    if (!plan_context_digest) {
        return core::Result<void>::failure(plan_context_digest.error());
    }

    GhostCorrectionReceipt correction;
    correction.kind = GhostCorrectionKind::vertex_position;
    correction.task_id = plan_.id;
    correction.object = object;
    correction.vertex = vertex;
    correction.before_position = current_vertex->position;
    correction.after_position = position;
    correction.before_mesh_digest = application::canonical_mesh_geometry_digest(mesh->second);
    correction.source_revision = plan_.objective.base_revision;
    correction.branch_revision_before = speculative_->document_.revision();
    correction.branch_revision_after = correction.branch_revision_before.next();
    correction.plan_context_digest = plan_context_digest.value();
    correction.parent_evidence_digest = ghost_corrections_.empty()
        ? assets::Sha256Digest{} : ghost_corrections_.back().evidence_digest;
    const auto anchor_step = ghost_correction_anchor(
        object, GhostCorrectionKind::vertex_position);
    if (!anchor_step) return core::Result<void>::failure(anchor_step.error());
    const auto dependent_steps = anchor_step.value().empty()
        ? std::vector<std::string>{} : dependent_plan_steps(anchor_step.value());
    if (!dependent_steps.empty() &&
        ghost_recompute_lineage_.size() >= envelope_.max_ghost_recomputations) {
        return core::Result<void>::failure(policy(
            "work envelope ghost-recompute budget is exhausted"));
    }
    correction.recompute_after_step_id = anchor_step.value();
    correction.justification = std::move(justification);

    auto selected_mode = application::HumanApplicationAccess::dispatch(
        *speculative_, application::SetSelectionModeAction{
            editor::SelectionMode::vertex});
    if (!selected_mode) return core::Result<void>::failure(selected_mode.error());
    auto selected_vertex = application::HumanApplicationAccess::dispatch(
        *speculative_, application::SelectVertexAction{object, vertex});
    if (!selected_vertex) return core::Result<void>::failure(selected_vertex.error());
    editor::ToolArguments arguments;
    arguments.position = position;
    const auto dispatched = application::HumanApplicationAccess::dispatch(
        *speculative_, application::InvokeToolAction{
            "mesh.set-vertex-position", arguments});
    if (!dispatched) {
        return core::Result<void>::failure(dispatched.error().with_context(
            "human ghost vertex correction"));
    }
    const auto* corrected_target = speculative_->document_.scene().find(object);
    const auto corrected_mesh = corrected_target != nullptr &&
            corrected_target->mesh_asset.has_value()
        ? speculative_->document_.meshes().find(*corrected_target->mesh_asset)
        : speculative_->document_.meshes().end();
    if (corrected_mesh == speculative_->document_.meshes().end()) {
        return core::Result<void>::failure(stale(
            "ghost vertex correction lost its target mesh after native execution"));
    }
    correction.after_mesh_digest =
        application::canonical_mesh_geometry_digest(corrected_mesh->second);
    const auto evidence_digest = correction.computed_evidence_digest();
    if (!evidence_digest) return core::Result<void>::failure(evidence_digest.error());
    correction.evidence_digest = evidence_digest.value();
    if (speculative_->document_.revision() != correction.branch_revision_after) {
        return core::Result<void>::failure(invalid_state(
            "ghost vertex correction did not advance exactly one speculative revision"));
    }
    const auto correction_evidence_digest = correction.evidence_digest;
    ghost_corrections_.push_back(std::move(correction));
    gates_ = evaluate_gates();
    cached_snapshot_.reset();
    return recompute_after_ghost_correction(
        correction_evidence_digest, anchor_step.value(), dependent_steps);
}

core::Result<void> AgentTask::correct_ghost_face(
    scene::ObjectId object,
    geometry::FaceId face,
    GhostCorrectionKind kind,
    double distance,
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
    if (kind != GhostCorrectionKind::face_inset &&
        kind != GhostCorrectionKind::face_extrusion) {
        return core::Result<void>::failure(invalid(
            "ghost face correction must select a native inset or extrusion"));
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
    if (!face || !std::isfinite(distance) || distance <= 0.0 ||
        distance > 1'000'000.0) {
        return core::Result<void>::failure(invalid(
            "ghost face correction requires a valid face and bounded positive distance"));
    }
    if (ghost_corrections_.size() >= 256U) {
        return core::Result<void>::failure(policy(
            "ghost correction evidence budget is exhausted"));
    }
    const std::uint32_t required_effects =
        ::carto::agent::effect_mask(Effect::geometry) |
        ::carto::agent::effect_mask(Effect::topology);
    if ((envelope_.permitted_effects & required_effects) != required_effects ||
        envelope_.max_topology_operations == 0U) {
        return core::Result<void>::failure(policy(
            "work envelope does not permit speculative topology correction"));
    }
    const auto topology_constraint = std::find_if(
        plan_.objective.hard_constraints.begin(), plan_.objective.hard_constraints.end(),
        [object](const HardConstraint& constraint) {
            return constraint.kind == ObjectiveConstraintKind::preserve_topology &&
                (!constraint.object.has_value() || *constraint.object == object);
        });
    if (topology_constraint != plan_.objective.hard_constraints.end()) {
        return core::Result<void>::failure(policy(
            "objective hard constraint " + topology_constraint->id +
            " forbids topology changes for this target"));
    }
    const auto* target = speculative_->document_.scene().find(object);
    if (target == nullptr || target->locked || !target->mesh_asset.has_value()) {
        return core::Result<void>::failure(policy(
            "ghost face correction requires an unlocked mesh object"));
    }
    const auto mesh = speculative_->document_.meshes().find(*target->mesh_asset);
    if (mesh == speculative_->document_.meshes().end()) {
        return core::Result<void>::failure(stale(
            "ghost face correction target has no current mesh asset"));
    }
    if (mesh->second.find_face(face) == nullptr) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "ghost correction face is absent from the speculative mesh"));
    }
    for (const auto& instance : speculative_->document_.scene().objects_sorted()) {
        if (instance.mesh_asset != target->mesh_asset) continue;
        if (!scope_contains(instance.id) ||
            !contains(plan_.objective.scope_objects, instance.id) || instance.locked ||
            contains(plan_.objective.protected_objects, instance.id)) {
            return core::Result<void>::failure(policy(
                "all instances of a corrected shared mesh must be in-scope, unlocked, and unprotected"));
        }
    }
    const auto plan_context_digest = plan_.context_digest();
    if (!plan_context_digest) {
        return core::Result<void>::failure(plan_context_digest.error());
    }

    GhostCorrectionReceipt correction;
    correction.kind = kind;
    correction.task_id = plan_.id;
    correction.object = object;
    correction.face = face;
    correction.distance = distance;
    correction.before_mesh_digest =
        application::canonical_mesh_geometry_digest(mesh->second);
    correction.source_revision = plan_.objective.base_revision;
    correction.branch_revision_before = speculative_->document_.revision();
    correction.branch_revision_after = correction.branch_revision_before.next();
    correction.plan_context_digest = plan_context_digest.value();
    correction.parent_evidence_digest = ghost_corrections_.empty()
        ? assets::Sha256Digest{} : ghost_corrections_.back().evidence_digest;
    const auto anchor_step = ghost_correction_anchor(object, kind);
    if (!anchor_step) return core::Result<void>::failure(anchor_step.error());
    const auto dependent_steps = anchor_step.value().empty()
        ? std::vector<std::string>{} : dependent_plan_steps(anchor_step.value());
    if (!dependent_steps.empty() &&
        ghost_recompute_lineage_.size() >= envelope_.max_ghost_recomputations) {
        return core::Result<void>::failure(policy(
            "work envelope ghost-recompute budget is exhausted"));
    }
    correction.recompute_after_step_id = anchor_step.value();
    correction.justification = std::move(justification);

    auto selected_mode = application::HumanApplicationAccess::dispatch(
        *speculative_, application::SetSelectionModeAction{
            editor::SelectionMode::face});
    if (!selected_mode) return core::Result<void>::failure(selected_mode.error());
    auto selected_face = application::HumanApplicationAccess::dispatch(
        *speculative_, application::SelectFaceAction{object, face});
    if (!selected_face) return core::Result<void>::failure(selected_face.error());
    editor::ToolArguments arguments;
    arguments.distance = distance;
    const std::string_view tool_id = kind == GhostCorrectionKind::face_inset
        ? "mesh.inset-face" : "mesh.extrude-face";
    const auto dispatched = application::HumanApplicationAccess::dispatch(
        *speculative_, application::InvokeToolAction{std::string(tool_id), arguments});
    if (!dispatched) {
        return core::Result<void>::failure(dispatched.error().with_context(
            "human ghost face correction"));
    }
    const auto* corrected_target = speculative_->document_.scene().find(object);
    const auto corrected_mesh = corrected_target != nullptr &&
            corrected_target->mesh_asset.has_value()
        ? speculative_->document_.meshes().find(*corrected_target->mesh_asset)
        : speculative_->document_.meshes().end();
    if (corrected_mesh == speculative_->document_.meshes().end()) {
        return core::Result<void>::failure(stale(
            "ghost face correction lost its target mesh after native execution"));
    }
    correction.after_mesh_digest =
        application::canonical_mesh_geometry_digest(corrected_mesh->second);
    const auto evidence_digest = correction.computed_evidence_digest();
    if (!evidence_digest) return core::Result<void>::failure(evidence_digest.error());
    correction.evidence_digest = evidence_digest.value();
    if (speculative_->document_.revision() != correction.branch_revision_after) {
        return core::Result<void>::failure(invalid_state(
            "ghost face correction did not advance exactly one speculative revision"));
    }
    const auto correction_evidence_digest = correction.evidence_digest;
    ghost_corrections_.push_back(std::move(correction));
    gates_ = evaluate_gates();
    cached_snapshot_.reset();
    return recompute_after_ghost_correction(
        correction_evidence_digest, anchor_step.value(), dependent_steps);
}

core::Result<std::string> AgentTask::ghost_correction_anchor(
    scene::ObjectId object,
    GhostCorrectionKind kind) const {
    if (!speculative_) {
        return core::Result<std::string>::failure(invalid_state(
            "ghost correction anchor requires a speculative branch"));
    }
    const auto* target = speculative_->document_.scene().find(object);
    if (target == nullptr) {
        return core::Result<std::string>::failure(stale(
            "ghost correction anchor target is missing"));
    }
    std::set<scene::ObjectId> affected_objects;
    if (kind == GhostCorrectionKind::object_transform) {
        for (const auto& candidate : speculative_->document_.scene().objects_sorted()) {
            const scene::SceneObject* current = &candidate;
            bool affected = candidate.id == object;
            std::set<scene::ObjectId> visited;
            while (!affected && current->parent.has_value()) {
                if (!visited.insert(current->id).second) {
                    return core::Result<std::string>::failure(invalid_state(
                        "ghost correction target hierarchy contains a cycle"));
                }
                if (*current->parent == object) {
                    affected = true;
                    break;
                }
                current = speculative_->document_.scene().find(*current->parent);
                if (current == nullptr) {
                    return core::Result<std::string>::failure(stale(
                        "ghost correction target hierarchy has a missing parent"));
                }
            }
            if (affected) {
                if (!scope_contains(candidate.id) ||
                    !contains(plan_.objective.scope_objects, candidate.id) ||
                    candidate.locked ||
                    contains(plan_.objective.protected_objects, candidate.id)) {
                    return core::Result<std::string>::failure(policy(
                        "root ghost transform would affect an out-of-scope, locked, or protected descendant"));
                }
                affected_objects.insert(candidate.id);
            }
        }
    } else if (kind == GhostCorrectionKind::vertex_position ||
        kind == GhostCorrectionKind::face_inset ||
        kind == GhostCorrectionKind::face_extrusion) {
        if (!target->mesh_asset.has_value()) {
            return core::Result<std::string>::failure(policy(
                "mesh ghost correction target has no mesh asset"));
        }
        for (const auto& candidate : speculative_->document_.scene().objects_sorted()) {
            if (candidate.mesh_asset != target->mesh_asset) continue;
            if (!scope_contains(candidate.id) ||
                !contains(plan_.objective.scope_objects, candidate.id) ||
                candidate.locked ||
                contains(plan_.objective.protected_objects, candidate.id)) {
                return core::Result<std::string>::failure(policy(
                    "ghost mesh correction would affect an out-of-scope, locked, or protected instance"));
            }
            affected_objects.insert(candidate.id);
        }
    } else {
        return core::Result<std::string>::failure(invalid(
            "ghost correction kind cannot be anchored to the plan"));
    }

    const auto schedule = plan_.schedule_order();
    if (!schedule) return core::Result<std::string>::failure(schedule.error());
    std::string anchor;
    for (const auto& step_id : schedule.value()) {
        const auto step = std::find_if(plan_.steps.begin(), plan_.steps.end(),
            [&step_id](const AgentPlanStep& candidate) { return candidate.id == step_id; });
        const auto progress = std::find_if(plan_progress_.begin(), plan_progress_.end(),
            [&step_id](const PlanStepProgress& candidate) {
                return candidate.step_id == step_id;
            });
        if (step != plan_.steps.end() && affected_objects.contains(step->target.object) &&
            progress != plan_progress_.end() && progress->status == StepStatus::passed) {
            anchor = step_id;
        }
    }
    return core::Result<std::string>::success(std::move(anchor));
}

core::Result<std::optional<std::string>> AgentTask::effective_ghost_anchor(
    const GhostCorrectionReceipt& correction) const {
    if (!correction.recompute_after_step_id.has_value() ||
        correction.recompute_after_step_id->empty()) {
        return core::Result<std::optional<std::string>>::success(std::nullopt);
    }
    const auto step = std::find_if(plan_.steps.begin(), plan_.steps.end(),
        [&correction](const AgentPlanStep& candidate) {
            return candidate.id == *correction.recompute_after_step_id;
        });
    if (step != plan_.steps.end()) {
        return core::Result<std::optional<std::string>>::success(step->id);
    }
    const auto current_context = plan_.context_digest();
    if (!current_context) {
        return core::Result<std::optional<std::string>>::failure(current_context.error());
    }
    if (correction.plan_context_digest == current_context.value()) {
        return core::Result<std::optional<std::string>>::failure(stale(
            "locked ghost correction anchor is absent from its bound plan"));
    }
    // A reviewed repair may replace the plan step. In that case the new plan
    // runs from source and the inherited human constraint is applied afterward.
    return core::Result<std::optional<std::string>>::success(std::nullopt);
}

std::vector<std::string> AgentTask::dependent_plan_steps(
    std::string_view anchor_step_id) const {
    const auto schedule = plan_.schedule_order();
    if (!schedule) return {};
    std::set<std::string> descendants{std::string(anchor_step_id)};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& step : plan_.steps) {
            if (descendants.contains(step.id)) continue;
            const bool depends_on_descendant = std::any_of(
                step.dependencies.begin(), step.dependencies.end(),
                [&descendants](const std::string& dependency) {
                    return descendants.contains(dependency);
                });
            if (depends_on_descendant) {
                descendants.insert(step.id);
                changed = true;
            }
        }
    }
    std::vector<std::string> result;
    for (const auto& step_id : schedule.value()) {
        if (step_id != anchor_step_id && descendants.contains(step_id)) {
            result.push_back(step_id);
        }
    }
    return result;
}

core::Result<void> AgentTask::validate_ghost_correction_lineage() const {
    if (ghost_corrections_.empty()) return core::Result<void>::success();
    const auto current_plan_context = plan_.context_digest();
    if (!current_plan_context) {
        return core::Result<void>::failure(current_plan_context.error());
    }
    assets::Sha256Digest expected_parent;
    std::map<scene::ObjectId, core::Transform> last_recorded_transforms;
    using GhostVertexKey = std::pair<std::uint64_t, geometry::VertexId>;
    std::map<GhostVertexKey, core::Vec3d> last_vertex_positions;
    std::map<scene::ObjectId, assets::Sha256Digest> last_mesh_digests;
    const auto shared_mesh_scope_valid = [this](const scene::SceneObject& target) {
        for (const auto& instance : speculative_->document_.scene().objects_sorted()) {
            if (instance.mesh_asset == target.mesh_asset &&
                (!scope_contains(instance.id) ||
                 !contains(plan_.objective.scope_objects, instance.id) || instance.locked ||
                 contains(plan_.objective.protected_objects, instance.id))) {
                return false;
            }
        }
        return true;
    };
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
        const auto anchor = effective_ghost_anchor(correction);
        if (!anchor) return core::Result<void>::failure(anchor.error());
        if (correction.parent_evidence_digest != expected_parent ||
            !source_revision_matches || !plan_context_matches ||
            !contains(plan_.objective.scope_objects, correction.object) ||
            !scope_contains(correction.object) ||
            contains(plan_.objective.protected_objects, correction.object)) {
            return core::Result<void>::failure(policy(
                "locked ghost correction lineage no longer matches the task scope"));
        }
        if (correction.kind == GhostCorrectionKind::object_transform) {
            const auto* target = speculative_->document_.scene().find(correction.object);
            if (target == nullptr || target->locked || target->parent.has_value()) {
                return core::Result<void>::failure(policy(
                    "locked ghost transform target is missing, locked, or no longer a root"));
            }
            const auto previous = last_recorded_transforms.find(correction.object);
            if (previous != last_recorded_transforms.end() &&
                !same_transform(previous->second, correction.before_transform)) {
                return core::Result<void>::failure(stale(
                    "locked ghost correction transform chain is discontinuous"));
            }
            last_recorded_transforms[correction.object] = correction.after_transform;
        } else if (correction.kind == GhostCorrectionKind::vertex_position &&
            correction.vertex.has_value()) {
            const auto* target = speculative_->document_.scene().find(correction.object);
            if (target == nullptr || target->locked || !target->mesh_asset.has_value()) {
                return core::Result<void>::failure(policy(
                    "locked ghost vertex target is missing, locked, or has no mesh"));
            }
            const auto mesh = speculative_->document_.meshes().find(*target->mesh_asset);
            if (mesh == speculative_->document_.meshes().end() ||
                !shared_mesh_scope_valid(*target)) {
                return core::Result<void>::failure(policy(
                    "locked ghost vertex correction escapes shared-mesh scope"));
            }
            const GhostVertexKey key{*target->mesh_asset, *correction.vertex};
            const auto previous = last_vertex_positions.find(key);
            if (previous != last_vertex_positions.end() &&
                !same_position(previous->second, correction.before_position)) {
                return core::Result<void>::failure(stale(
                    "locked ghost vertex correction chain is discontinuous"));
            }
            last_vertex_positions[key] = correction.after_position;
            if (!correction.before_mesh_digest.is_zero()) {
                const auto previous_mesh = last_mesh_digests.find(correction.object);
                if (previous_mesh != last_mesh_digests.end() &&
                    previous_mesh->second != correction.before_mesh_digest) {
                    return core::Result<void>::failure(stale(
                        "locked ghost mesh correction chain is discontinuous"));
                }
                last_mesh_digests[correction.object] = correction.after_mesh_digest;
            }
        } else if (correction.kind == GhostCorrectionKind::face_inset ||
            correction.kind == GhostCorrectionKind::face_extrusion) {
            const auto* target = speculative_->document_.scene().find(correction.object);
            if (target == nullptr || target->locked || !target->mesh_asset.has_value() ||
                !correction.face.has_value() || !shared_mesh_scope_valid(*target)) {
                return core::Result<void>::failure(policy(
                    "locked ghost face correction escapes its mesh or shared-mesh scope"));
            }
            const auto previous_mesh = last_mesh_digests.find(correction.object);
            if (previous_mesh != last_mesh_digests.end() &&
                previous_mesh->second != correction.before_mesh_digest) {
                return core::Result<void>::failure(stale(
                    "locked ghost mesh correction chain is discontinuous"));
            }
            last_mesh_digests[correction.object] = correction.after_mesh_digest;
        } else {
            return core::Result<void>::failure(invalid(
                "locked ghost correction kind is unknown"));
        }
        expected_parent = correction.evidence_digest;
    }
    return core::Result<void>::success();
}

core::Result<void> AgentTask::apply_ghost_corrections(
    std::optional<std::string_view> after_step_id) {
    if (ghost_corrections_.empty()) {
        if (!after_step_id.has_value()) replay_ghost_corrections_ = false;
        return core::Result<void>::success();
    }
    if (auto valid = validate_ghost_correction_lineage(); !valid) return valid;
    const auto shared_mesh_scope_valid = [this](const scene::SceneObject& target) {
        for (const auto& instance : speculative_->document_.scene().objects_sorted()) {
            if (instance.mesh_asset == target.mesh_asset &&
                (!scope_contains(instance.id) ||
                 !contains(plan_.objective.scope_objects, instance.id) || instance.locked ||
                 contains(plan_.objective.protected_objects, instance.id))) {
                return false;
            }
        }
        return true;
    };
    for (const auto& correction : ghost_corrections_) {
        const auto anchor = effective_ghost_anchor(correction);
        if (!anchor) return core::Result<void>::failure(anchor.error());
        const bool selected_for_replay = after_step_id.has_value()
            ? anchor.value().has_value() && *anchor.value() == *after_step_id
            : !anchor.value().has_value();
        if (!selected_for_replay) continue;

        const auto revision_before = speculative_->document_.revision();
        if (correction.kind == GhostCorrectionKind::object_transform) {
            const auto* current = speculative_->document_.scene().find(correction.object);
            if (current == nullptr || current->locked || current->parent.has_value() ||
                !same_transform(current->local_transform, correction.before_transform)) {
                return core::Result<void>::failure(stale(
                    "replayed plan changed the locked ghost transform correction chain"));
            }
            for (const auto& candidate : speculative_->document_.scene().objects_sorted()) {
                const scene::SceneObject* ancestor = &candidate;
                bool affected = candidate.id == correction.object;
                std::set<scene::ObjectId> visited;
                while (!affected && ancestor->parent.has_value()) {
                    if (!visited.insert(ancestor->id).second) {
                        return core::Result<void>::failure(invalid_state(
                            "ghost correction target hierarchy contains a cycle"));
                    }
                    if (*ancestor->parent == correction.object) {
                        affected = true;
                        break;
                    }
                    ancestor = speculative_->document_.scene().find(*ancestor->parent);
                    if (ancestor == nullptr) {
                        return core::Result<void>::failure(stale(
                            "ghost correction target hierarchy has a missing parent"));
                    }
                }
                if (affected && (!scope_contains(candidate.id) || candidate.locked ||
                    contains(plan_.objective.protected_objects, candidate.id))) {
                    return core::Result<void>::failure(policy(
                        "locked ghost transform replay would affect an unsafe descendant"));
                }
            }
            const auto dispatched = application::HumanApplicationAccess::dispatch(
                *speculative_, application::SetObjectTransformAction{
                    correction.object, correction.after_transform, revision_before});
            if (!dispatched) {
                return core::Result<void>::failure(dispatched.error().with_context(
                    "replaying locked ghost correction"));
            }
        } else if (correction.kind == GhostCorrectionKind::vertex_position) {
            const auto* current = speculative_->document_.scene().find(correction.object);
            const auto mesh = current != nullptr && current->mesh_asset.has_value()
                ? speculative_->document_.meshes().find(*current->mesh_asset)
                : speculative_->document_.meshes().end();
            const auto* vertex = correction.vertex.has_value() &&
                    mesh != speculative_->document_.meshes().end()
                ? mesh->second.find_vertex(*correction.vertex) : nullptr;
            if (current == nullptr || current->locked || vertex == nullptr ||
                !same_position(vertex->position, correction.before_position) ||
                (!correction.before_mesh_digest.is_zero() &&
                 application::canonical_mesh_geometry_digest(mesh->second) !=
                     correction.before_mesh_digest) || !shared_mesh_scope_valid(*current)) {
                return core::Result<void>::failure(stale(
                    "replaying plan changed the locked ghost vertex correction chain"));
            }
            auto mode = application::HumanApplicationAccess::dispatch(
                *speculative_, application::SetSelectionModeAction{
                    editor::SelectionMode::vertex});
            if (!mode) return core::Result<void>::failure(mode.error());
            auto selected = application::HumanApplicationAccess::dispatch(
                *speculative_, application::SelectVertexAction{
                    correction.object, *correction.vertex});
            if (!selected) return core::Result<void>::failure(selected.error());
            editor::ToolArguments arguments;
            arguments.position = correction.after_position;
            const auto dispatched = application::HumanApplicationAccess::dispatch(
                *speculative_, application::InvokeToolAction{
                    "mesh.set-vertex-position", arguments});
            if (!dispatched) {
                return core::Result<void>::failure(dispatched.error().with_context(
                    "replaying locked ghost vertex correction"));
            }
            const auto* after_target = speculative_->document_.scene().find(correction.object);
            const auto after_mesh = after_target != nullptr && after_target->mesh_asset.has_value()
                ? speculative_->document_.meshes().find(*after_target->mesh_asset)
                : speculative_->document_.meshes().end();
            if (!correction.after_mesh_digest.is_zero() &&
                (after_mesh == speculative_->document_.meshes().end() ||
                 application::canonical_mesh_geometry_digest(after_mesh->second) !=
                     correction.after_mesh_digest)) {
                return core::Result<void>::failure(stale(
                    "replayed ghost vertex correction produced a different mesh"));
            }
        } else {
            const auto* current = speculative_->document_.scene().find(correction.object);
            const auto mesh = current != nullptr && current->mesh_asset.has_value()
                ? speculative_->document_.meshes().find(*current->mesh_asset)
                : speculative_->document_.meshes().end();
            if (current == nullptr || current->locked ||
                mesh == speculative_->document_.meshes().end() ||
                !correction.face.has_value() ||
                mesh->second.find_face(*correction.face) == nullptr ||
                application::canonical_mesh_geometry_digest(mesh->second) !=
                    correction.before_mesh_digest || !shared_mesh_scope_valid(*current)) {
                return core::Result<void>::failure(stale(
                    "replaying plan changed the locked ghost face correction target"));
            }
            auto mode = application::HumanApplicationAccess::dispatch(
                *speculative_, application::SetSelectionModeAction{
                    editor::SelectionMode::face});
            if (!mode) return core::Result<void>::failure(mode.error());
            auto selected = application::HumanApplicationAccess::dispatch(
                *speculative_, application::SelectFaceAction{
                    correction.object, *correction.face});
            if (!selected) return core::Result<void>::failure(selected.error());
            editor::ToolArguments arguments;
            arguments.distance = correction.distance;
            const std::string_view tool_id =
                correction.kind == GhostCorrectionKind::face_inset
                    ? "mesh.inset-face" : "mesh.extrude-face";
            const auto dispatched = application::HumanApplicationAccess::dispatch(
                *speculative_, application::InvokeToolAction{
                    std::string(tool_id), arguments});
            if (!dispatched) {
                return core::Result<void>::failure(dispatched.error().with_context(
                    "replaying locked ghost face correction"));
            }
            const auto* after_target = speculative_->document_.scene().find(correction.object);
            const auto after_mesh = after_target != nullptr && after_target->mesh_asset.has_value()
                ? speculative_->document_.meshes().find(*after_target->mesh_asset)
                : speculative_->document_.meshes().end();
            if (after_mesh == speculative_->document_.meshes().end() ||
                application::canonical_mesh_geometry_digest(after_mesh->second) !=
                    correction.after_mesh_digest) {
                return core::Result<void>::failure(stale(
                    "replayed ghost face correction produced a different mesh"));
            }
        }
        if (speculative_->document_.revision() != revision_before.next()) {
            return core::Result<void>::failure(invalid_state(
                "replaying a locked ghost correction did not advance one speculative revision"));
        }
    }
    if (!after_step_id.has_value()) replay_ghost_corrections_ = false;
    return core::Result<void>::success();
}

core::Result<void> AgentTask::recompute_after_ghost_correction(
    const assets::Sha256Digest& correction_evidence_digest,
    std::string anchor_step_id,
    std::vector<std::string> dependent_step_ids) {
    if (dependent_step_ids.empty()) {
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
                "human ghost correction leaves one or more required gates or plan steps blocked");
        }
        cached_snapshot_.reset();
        return core::Result<void>::success();
    }
    if (!source_ || !speculative_ ||
        ghost_recompute_lineage_.size() >= envelope_.max_ghost_recomputations) {
        status_ = TaskStatus::blocked;
        diagnostic_ = policy("work envelope ghost-recompute budget is exhausted");
        cached_snapshot_.reset();
        return core::Result<void>::success();
    }

    AgentGhostRecomputeReceipt recompute;
    recompute.task_id = plan_.id;
    recompute.anchor_step_id = std::move(anchor_step_id);
    recompute.correction_evidence_digest = correction_evidence_digest;
    recompute.dependent_step_ids = std::move(dependent_step_ids);
    recompute.previous_step_receipts = receipts_;
    recompute.previous_gate_results = gates_;
    recompute.previous_diff_digest = build_diff().digest;
    recompute.previous_speculative_revision = speculative_->document_.revision();

    const auto record_failed_replay = [this, &recompute](core::Diagnostic diagnostic) {
        recompute.replay_completed = false;
        recompute.diagnostic = diagnostic;
        recompute.result_diff_digest = build_diff().digest;
        recompute.result_speculative_revision = speculative_->document_.revision();
        const auto evidence = recompute.computed_evidence_digest();
        if (evidence) {
            recompute.evidence_digest = evidence.value();
            ghost_recompute_lineage_.push_back(recompute);
        }
        status_ = TaskStatus::blocked;
        diagnostic_ = std::move(diagnostic);
        cached_snapshot_.reset();
        return core::Result<void>::success();
    };

    auto candidate_result = AgentTask::begin(*source_, plan_, envelope_);
    if (!candidate_result) {
        return record_failed_replay(candidate_result.error().with_context(
            "rebuilding the speculative branch after a human ghost correction"));
    }
    auto candidate = std::move(candidate_result.value());
    candidate.repairs_ = repairs_;
    candidate.rebases_ = rebases_;
    candidate.ghost_corrections_ = ghost_corrections_;
    candidate.ghost_recompute_lineage_ = ghost_recompute_lineage_;
    candidate.replay_ghost_corrections_ = true;
    const auto replayed = candidate.run();
    if (!replayed) {
        return record_failed_replay(replayed.error().with_context(
            "replaying the dependent plan after a human ghost correction"));
    }
    if (candidate.ghost_correction_replay_failed_) {
        return record_failed_replay(candidate.diagnostic_.value_or(stale(
            "human ghost correction could not be replayed at its bound plan step")));
    }

    recompute.replay_completed = true;
    recompute.diagnostic = candidate.diagnostic_;
    recompute.result_diff_digest = candidate.build_diff().digest;
    recompute.result_speculative_revision = candidate.speculative_->document_.revision();
    const auto recompute_digest = recompute.computed_evidence_digest();
    if (!recompute_digest) {
        return record_failed_replay(recompute_digest.error().with_context(
            "binding ghost recompute lineage"));
    }
    recompute.evidence_digest = recompute_digest.value();
    candidate.ghost_recompute_lineage_.push_back(std::move(recompute));

    speculative_ = std::move(candidate.speculative_);
    receipts_ = std::move(candidate.receipts_);
    repairs_ = std::move(candidate.repairs_);
    rebases_ = std::move(candidate.rebases_);
    ghost_corrections_ = std::move(candidate.ghost_corrections_);
    ghost_recompute_lineage_ = std::move(candidate.ghost_recompute_lineage_);
    replay_ghost_corrections_ = candidate.replay_ghost_corrections_;
    ghost_correction_replay_failed_ = candidate.ghost_correction_replay_failed_;
    plan_progress_ = std::move(candidate.plan_progress_);
    gates_ = std::move(candidate.gates_);
    query_budget_ = candidate.query_budget_;
    touched_objects_ = std::move(candidate.touched_objects_);
    transform_distance_used_meters_ = candidate.transform_distance_used_meters_;
    status_ = candidate.status_;
    diagnostic_ = std::move(candidate.diagnostic_);
    cached_snapshot_.reset();
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
    const bool material_assignment = intent_starts_with(
        step.intent, "assign", "material");
    const bool lightmap_uv_generation = intent_starts_with(
        step.intent, "generate", "lightmap", "uv");
    if (material_assignment &&
        (envelope_.permitted_effects & ::carto::agent::effect_mask(Effect::material)) == 0U) {
        return fail(policy("work envelope forbids the material effect"));
    }
    if (lightmap_uv_generation &&
        (envelope_.permitted_effects & ::carto::agent::effect_mask(Effect::uv)) == 0U) {
        return fail(policy("work envelope forbids the UV effect"));
    }
    if (material_assignment &&
        !std::holds_alternative<std::monostate>(step.target.component)) {
        return fail(policy("material assignment requires an object target"));
    }
    if (lightmap_uv_generation &&
        !std::holds_alternative<std::monostate>(step.target.component)) {
        return fail(policy("lightmap UV generation requires an object target"));
    }
    const auto* target_object = speculative_->document_.scene().find(step.target.object);
    if (target_object == nullptr) {
        return fail(policy("plan target object no longer exists in the speculative branch"));
    }
    const bool object_translation =
        std::holds_alternative<std::monostate>(step.target.component) &&
        !material_assignment && !lightmap_uv_generation;
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
            if ((material_assignment || lightmap_uv_generation) && object.locked) {
                return fail(policy(
                    "mesh authoring would affect a locked shared-mesh instance"));
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
    if (object_translation || material_assignment || lightmap_uv_generation) {
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
        *speculative_, preview.value(), (material_assignment || lightmap_uv_generation)
            ? application::OperationSource::agent_workbench
            : application::OperationSource::ai_proposal,
        plan_.id + ":" + step.id);
    if (!committed) return fail(committed.error().with_context("speculative native command"));
    if (auto result = speculative_->document_.validate(); !result) {
        return fail(result.error().with_context("speculative project validation"));
    }

    touched_objects_.insert(affected_objects.begin(), affected_objects.end());
    transform_distance_used_meters_ += transform_step_distance;
    step_transform_distance_meters_[step.id] = transform_step_distance;
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
    ghost_correction_replay_failed_ = false;
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
        if (rebase_reused_step_ids_.contains(step_id)) {
            progress_it->status = StepStatus::ready;
            progress_it->status = StepStatus::running;
            const auto reused_receipt = rebase_reused_step_receipts_.find(step_id);
            if (!rebase_source_document_.has_value() ||
                !rebase_speculative_document_.has_value() ||
                reused_receipt == rebase_reused_step_receipts_.end() ||
                reused_receipt->second.status != StepStatus::passed || rebases_.empty()) {
                status_ = TaskStatus::blocked;
                diagnostic_ = invalid_state(
                    "rebase reuse is missing its verified historical step evidence");
                break;
            }
            std::vector<scene::ObjectId> step_write_objects;
            std::vector<std::uint64_t> step_write_mesh_assets;
            if (std::holds_alternative<std::monostate>(step_it->target.component)) {
                std::set<scene::ObjectId> writes;
                for (const auto& object :
                     rebase_source_document_->scene().objects_sorted()) {
                    const scene::SceneObject* ancestor = &object;
                    std::set<scene::ObjectId> visited;
                    for (std::size_t depth = 0U; ancestor != nullptr &&
                         ancestor->parent.has_value(); ++depth) {
                        const scene::ObjectId parent = *ancestor->parent;
                        if (depth >= 4096U || !visited.insert(parent).second) {
                            status_ = TaskStatus::blocked;
                            diagnostic_ = invalid_state(
                                "rebase reuse found an invalid transform hierarchy");
                            break;
                        }
                        if (parent == step_it->target.object) {
                            writes.insert(object.id);
                            break;
                        }
                        ancestor = rebase_source_document_->scene().find(parent);
                    }
                    if (status_ == TaskStatus::blocked) break;
                    if (object.id == step_it->target.object) writes.insert(object.id);
                }
                if (status_ == TaskStatus::blocked) break;
                step_write_objects.assign(writes.begin(), writes.end());
            } else {
                const auto* target = rebase_source_document_->scene().find(
                    step_it->target.object);
                if (target == nullptr || !target->mesh_asset.has_value()) {
                    status_ = TaskStatus::blocked;
                    diagnostic_ = stale(
                        "reused mesh step target disappeared from its historical source");
                    break;
                }
                step_write_mesh_assets.push_back(*target->mesh_asset);
            }
            const auto& historical_receipt = reused_receipt->second;
            if (historical_receipt.revision_after <= historical_receipt.revision_before) {
                status_ = TaskStatus::blocked;
                diagnostic_ = invalid_state(
                    "reused step has no positive historical revision delta");
                break;
            }
            const std::uint64_t revision_delta =
                historical_receipt.revision_after.value() -
                historical_receipt.revision_before.value();
            const auto branch_revision = speculative_->document_.revision();
            auto candidate = speculative_->merge_agent_rebase_step_document(
                *rebase_source_document_, *rebase_speculative_document_,
                step_write_objects, step_write_mesh_assets, revision_delta);
            if (!candidate) {
                progress_it->status = StepStatus::failed;
                progress_it->diagnostic = candidate.error();
                status_ = TaskStatus::blocked;
                diagnostic_ = candidate.error().with_context(
                    "merging a verified unaffected plan step");
                break;
            }
            const auto installed = speculative_->install_agent_rebase_branch(
                candidate.value(), branch_revision, plan_.id,
                rebases_.back().checkpoint_plan_context_digest.hex(),
                rebases_.back().checkpoint_source_digest.hex());
            if (!installed) {
                progress_it->status = StepStatus::failed;
                progress_it->diagnostic = installed.error();
                status_ = TaskStatus::blocked;
                diagnostic_ = installed.error().with_context(
                    "installing a verified unaffected plan step");
                break;
            }
            touched_objects_.insert(
                step_write_objects.begin(), step_write_objects.end());
            for (const std::uint64_t asset : step_write_mesh_assets) {
                for (const auto& instance :
                     rebase_source_document_->scene().objects_sorted()) {
                    if (instance.mesh_asset == asset) touched_objects_.insert(instance.id);
                }
            }
            const auto reused_distance = step_transform_distance_meters_.find(step_id);
            const double distance = reused_distance == step_transform_distance_meters_.end()
                ? 0.0 : reused_distance->second;
            if (!std::isfinite(distance) || distance < 0.0 ||
                transform_distance_used_meters_ >
                    envelope_.max_transform_distance_meters ||
                distance > envelope_.max_transform_distance_meters -
                    transform_distance_used_meters_) {
                progress_it->status = StepStatus::failed;
                progress_it->diagnostic = policy(
                    "reused steps exceed the work envelope transform-distance budget");
                status_ = TaskStatus::blocked;
                diagnostic_ = progress_it->diagnostic;
                break;
            }
            transform_distance_used_meters_ += distance;
            progress_it->status = StepStatus::passed;
            receipts_.push_back(historical_receipt);
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
        if (result && replay_ghost_corrections_) {
            const auto replayed = apply_ghost_corrections(step_id);
            if (!replayed) {
                ghost_correction_replay_failed_ = true;
                status_ = TaskStatus::blocked;
                diagnostic_ = replayed.error();
                break;
            }
        }
    }
    if (status_ == TaskStatus::running && replay_ghost_corrections_) {
        for (const auto& correction : ghost_corrections_) {
            const auto anchor = effective_ghost_anchor(correction);
            if (!anchor) {
                ghost_correction_replay_failed_ = true;
                status_ = TaskStatus::blocked;
                diagnostic_ = anchor.error();
                break;
            }
            if (!anchor.value().has_value()) continue;
            const auto progress = std::find_if(
                plan_progress_.begin(), plan_progress_.end(), [&anchor](const auto& item) {
                    return item.step_id == *anchor.value();
                });
            if (progress == plan_progress_.end() || progress->status != StepStatus::passed) {
                ghost_correction_replay_failed_ = true;
                status_ = TaskStatus::blocked;
                diagnostic_ = stale(
                    "locked ghost correction anchor did not pass during plan replay");
                break;
            }
        }
        if (status_ == TaskStatus::running) {
            if (auto corrected = apply_ghost_corrections(); !corrected) {
                ghost_correction_replay_failed_ = true;
                status_ = TaskStatus::blocked;
                diagnostic_ = corrected.error();
            }
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

ProjectDiff ProjectDiff::between(
    const project::ProjectDocument& base_document_,
    const project::ProjectDocument& speculative_document) {
    ProjectDiff diff;
    diff.base_revision = base_document_.revision();
    diff.speculative_revision = speculative_document.revision();

    const auto before_objects = base_document_.scene().objects_sorted();
    const auto after_objects = speculative_document.scene().objects_sorted();
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
        if (object_has_changes(change)) diff.objects.push_back(change);
    }

    const auto& before_meshes = base_document_.meshes();
    const auto& after_meshes = speculative_document.meshes();
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

            std::set<std::string> attribute_layer_ids;
            for (const auto& [layer_id, layer] : before->second.attributes().layers()) {
                static_cast<void>(layer);
                attribute_layer_ids.insert(layer_id);
            }
            for (const auto& [layer_id, layer] : after->second.attributes().layers()) {
                static_cast<void>(layer);
                attribute_layer_ids.insert(layer_id);
            }
            for (const auto& layer_id : attribute_layer_ids) {
                const auto* before_layer = before->second.attributes().find(layer_id);
                const auto* after_layer = after->second.attributes().find(layer_id);
                AttributeLayerChange layer_change;
                layer_change.layer_id = layer_id;
                layer_change.added = before_layer == nullptr;
                layer_change.removed = after_layer == nullptr;
                const auto* descriptor_layer = after_layer != nullptr
                    ? after_layer : before_layer;
                if (descriptor_layer == nullptr) continue;
                layer_change.domain = descriptor_layer->descriptor.domain;
                layer_change.type = descriptor_layer->descriptor.type;
                layer_change.before_value_count = before_layer == nullptr
                    ? 0U : before_layer->values.size();
                layer_change.after_value_count = after_layer == nullptr
                    ? 0U : after_layer->values.size();
                if (before_layer == nullptr) {
                    layer_change.changed_value_count = layer_change.after_value_count;
                } else if (after_layer == nullptr) {
                    layer_change.changed_value_count = layer_change.before_value_count;
                } else {
                    layer_change.descriptor_changed =
                        before_layer->descriptor.domain != after_layer->descriptor.domain ||
                        before_layer->descriptor.type != after_layer->descriptor.type;
                    const std::size_t value_count = std::max(
                        before_layer->values.size(), after_layer->values.size());
                    for (std::size_t index = 0U; index < value_count; ++index) {
                        if (index >= before_layer->values.size() ||
                            index >= after_layer->values.size() ||
                            layer_change.descriptor_changed ||
                            !same_attribute_value(
                                before_layer->values[index], after_layer->values[index])) {
                            ++layer_change.changed_value_count;
                        }
                    }
                }
                if (layer_change.added || layer_change.removed ||
                    layer_change.descriptor_changed || layer_change.changed_value_count > 0U) {
                    change.attribute_layers.push_back(std::move(layer_change));
                }
            }

            std::set<attributes::UvSetId> uv_set_ids;
            for (const auto& [uv_set_id, descriptor] : before->second.uv_sets().descriptors()) {
                static_cast<void>(descriptor);
                uv_set_ids.insert(uv_set_id);
            }
            for (const auto& [uv_set_id, descriptor] : after->second.uv_sets().descriptors()) {
                static_cast<void>(descriptor);
                uv_set_ids.insert(uv_set_id);
            }
            for (const auto uv_set_id : uv_set_ids) {
                const auto* before_descriptor = before->second.uv_sets().find(uv_set_id);
                const auto* after_descriptor = after->second.uv_sets().find(uv_set_id);
                UvSetChange uv_change;
                uv_change.id = uv_set_id;
                if (before_descriptor != nullptr) uv_change.before = *before_descriptor;
                if (after_descriptor != nullptr) uv_change.after = *after_descriptor;
                const auto before_values = before_descriptor == nullptr
                    ? std::optional<std::map<geometry::CornerId, core::Vec2d>>(
                        std::map<geometry::CornerId, core::Vec2d>{})
                    : uv_values_by_corner(before->second, before_descriptor->layer_id);
                const auto after_values = after_descriptor == nullptr
                    ? std::optional<std::map<geometry::CornerId, core::Vec2d>>(
                        std::map<geometry::CornerId, core::Vec2d>{})
                    : uv_values_by_corner(after->second, after_descriptor->layer_id);
                uv_change.details_available = before_values.has_value() &&
                    after_values.has_value();
                if (uv_change.details_available) {
                    std::set<geometry::CornerId> corner_ids;
                    for (const auto& [corner_id, value] : *before_values) {
                        static_cast<void>(value);
                        corner_ids.insert(corner_id);
                    }
                    for (const auto& [corner_id, value] : *after_values) {
                        static_cast<void>(value);
                        corner_ids.insert(corner_id);
                    }
                    for (const auto corner_id : corner_ids) {
                        const auto old_value = before_values->find(corner_id);
                        const auto new_value = after_values->find(corner_id);
                        const bool old_present = old_value != before_values->end();
                        const bool new_present = new_value != after_values->end();
                        if (old_present && new_present &&
                            old_value->second.x == new_value->second.x &&
                            old_value->second.y == new_value->second.y) {
                            continue;
                        }
                        ++uv_change.changed_corner_count;
                        if (uv_change.samples.size() < kMaxProjectDiffUvSamples) {
                            UvCornerValueChange sample;
                            sample.corner = corner_id;
                            if (old_present) sample.before = old_value->second;
                            if (new_present) sample.after = new_value->second;
                            uv_change.samples.push_back(std::move(sample));
                        }
                    }
                    uv_change.samples_truncated = uv_change.changed_corner_count >
                        uv_change.samples.size();
                }
                uv_change.descriptor_changed =
                    (before_descriptor == nullptr) != (after_descriptor == nullptr) ||
                    (before_descriptor != nullptr && after_descriptor != nullptr &&
                        !same_uv_set_descriptor(*before_descriptor, *after_descriptor));
                if (uv_change.descriptor_changed || uv_change.changed_corner_count > 0U ||
                    !uv_change.details_available) {
                    change.uv_sets.push_back(std::move(uv_change));
                }
            }
        }
        if (change.added || change.removed) {
            const geometry::EditableMesh& one_sided_mesh = change.added
                ? after->second : before->second;
            for (const auto& [layer_id, layer] : one_sided_mesh.attributes().layers()) {
                AttributeLayerChange layer_change;
                layer_change.layer_id = layer_id;
                layer_change.domain = layer.descriptor.domain;
                layer_change.type = layer.descriptor.type;
                layer_change.added = change.added;
                layer_change.removed = change.removed;
                layer_change.before_value_count = change.added ? 0U : layer.values.size();
                layer_change.after_value_count = change.added ? layer.values.size() : 0U;
                layer_change.changed_value_count = layer.values.size();
                change.attribute_layers.push_back(std::move(layer_change));
            }
            for (const auto& [uv_set_id, descriptor] :
                    one_sided_mesh.uv_sets().descriptors()) {
                UvSetChange uv_change;
                uv_change.id = uv_set_id;
                uv_change.descriptor_changed = true;
                if (change.added) uv_change.after = descriptor;
                else uv_change.before = descriptor;
                const auto values = uv_values_by_corner(one_sided_mesh, descriptor.layer_id);
                uv_change.details_available = values.has_value();
                if (values.has_value()) {
                    uv_change.changed_corner_count = values->size();
                    for (const auto& [corner_id, value] : *values) {
                        if (uv_change.samples.size() >= kMaxProjectDiffUvSamples) break;
                        UvCornerValueChange sample;
                        sample.corner = corner_id;
                        if (change.added) sample.after = value;
                        else sample.before = value;
                        uv_change.samples.push_back(std::move(sample));
                    }
                    uv_change.samples_truncated = uv_change.changed_corner_count >
                        uv_change.samples.size();
                }
                change.uv_sets.push_back(std::move(uv_change));
            }
        }
        if (change.added || change.removed ||
            change.before_revision != change.after_revision ||
            !change.attribute_layers.empty() || !change.uv_sets.empty()) {
            diff.meshes.push_back(std::move(change));
        }
    }
    for (const scene::ObjectId id : object_ids) {
        const auto* before_object = base_document_.scene().find(id);
        const auto* after_object = speculative_document.scene().find(id);
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
    const auto& before_assignments =
        base_document_.material_catalog().assignments();
    const auto& after_assignments =
        speculative_document.material_catalog().assignments();
    std::set<project::MaterialRegionKey> material_region_keys;
    for (const auto& [key, assignment] : before_assignments) {
        static_cast<void>(assignment);
        material_region_keys.insert(key);
    }
    for (const auto& [key, assignment] : after_assignments) {
        static_cast<void>(assignment);
        material_region_keys.insert(key);
    }
    const auto material_name = [](
            const project::MaterialCatalog& catalog,
            const project::MaterialRegionAssignment& assignment)
            -> std::optional<std::string> {
        const auto found = catalog.materials().find(assignment.material);
        return found == catalog.materials().end()
            ? std::nullopt : std::optional<std::string>(found->second.name);
    };
    for (const auto key : material_region_keys) {
        const auto before_assignment = before_assignments.find(key);
        const auto after_assignment = after_assignments.find(key);
        MaterialBindingChange change;
        change.key = key;
        change.added = before_assignment == before_assignments.end();
        change.removed = after_assignment == after_assignments.end();
        if (!change.added) {
            change.before = before_assignment->second;
            change.before_material_name = material_name(
                base_document_.material_catalog(), before_assignment->second);
        }
        if (!change.removed) {
            change.after = after_assignment->second;
            change.after_material_name = material_name(
                speculative_document.material_catalog(), after_assignment->second);
        }
        if (!change.added && !change.removed) {
            change.material_changed = before_assignment->second.material !=
                after_assignment->second.material;
            change.variant_changed = before_assignment->second.variant !=
                after_assignment->second.variant;
        }
        if (change.added || change.removed || change.material_changed ||
            change.variant_changed) {
            diff.material_bindings.push_back(std::move(change));
        }
    }
    const auto& before_materials = base_document_.material_catalog().materials();
    const auto& after_materials = speculative_document.material_catalog().materials();
    std::set<render::MaterialId> material_ids;
    for (const auto& [id, material] : before_materials) {
        static_cast<void>(material);
        material_ids.insert(id);
    }
    for (const auto& [id, material] : after_materials) {
        static_cast<void>(material);
        material_ids.insert(id);
    }
    for (const render::MaterialId id : material_ids) {
        const auto before_material = before_materials.find(id);
        const auto after_material = after_materials.find(id);
        MaterialSourceChange change;
        change.id = id;
        change.added = before_material == before_materials.end();
        change.removed = after_material == after_materials.end();
        if (!change.added) {
            change.before_name = before_material->second.name;
            change.before_revision = before_material->second.revision;
        }
        if (!change.removed) {
            change.after_name = after_material->second.name;
            change.after_revision = after_material->second.revision;
        }
        if (change.added) {
            change.source_changed = true;
            change.definition_changed = true;
            change.texture_dependencies_changed = material_has_texture_dependencies(
                after_material->second.definition);
        } else if (change.removed) {
            change.source_changed = true;
            change.definition_changed = true;
            change.texture_dependencies_changed = material_has_texture_dependencies(
                before_material->second.definition);
        } else {
            change.definition_changed = !same_material_surface_definition(
                before_material->second.definition, after_material->second.definition);
            change.source_changed = !same_material_source(
                before_material->second, after_material->second);
            change.texture_dependencies_changed = !same_material_texture_dependencies(
                before_material->second.definition, after_material->second.definition);
        }
        if (change.added || change.removed || change.source_changed) {
            diff.material_sources.push_back(std::move(change));
        }
    }

    const auto& before_textures = base_document_.material_catalog().textures();
    const auto& after_textures = speculative_document.material_catalog().textures();
    std::set<assets::TextureAssetId> texture_ids;
    for (const auto& [id, texture] : before_textures) {
        static_cast<void>(texture);
        texture_ids.insert(id);
    }
    for (const auto& [id, texture] : after_textures) {
        static_cast<void>(texture);
        texture_ids.insert(id);
    }
    for (const assets::TextureAssetId id : texture_ids) {
        const auto before_texture = before_textures.find(id);
        const auto after_texture = after_textures.find(id);
        TextureSourceChange change;
        change.id = id;
        change.added = before_texture == before_textures.end();
        change.removed = after_texture == after_textures.end();
        if (!change.added) {
            change.before_name = before_texture->second.name;
            change.before_content_digest = before_texture->second.content_digest;
        }
        if (!change.removed) {
            change.after_name = after_texture->second.name;
            change.after_content_digest = after_texture->second.content_digest;
        }
        if (change.added || change.removed) {
            change.source_changed = true;
            change.content_changed = true;
            change.metadata_changed = true;
        } else {
            change.source_changed = !same_texture_source(
                before_texture->second, after_texture->second);
            change.content_changed = !same_texture_source_content(
                before_texture->second, after_texture->second);
            change.metadata_changed = !same_texture_source_metadata(
                before_texture->second, after_texture->second);
        }
        if (change.source_changed) diff.texture_sources.push_back(std::move(change));
    }
    diff.material_catalog_changed = base_document_.material_catalog().serialize() !=
        speculative_document.material_catalog().serialize();
    diff.engineering_changed = base_document_.engineering_authoring() !=
        speculative_document.engineering_authoring();
    diff.condition_changed = base_document_.condition_authoring() !=
        speculative_document.condition_authoring();
    diff.digest = project_result_digest(speculative_document);
    return diff;
}

ProjectDiff AgentTask::build_diff() const {
    return ProjectDiff::between(base_document_, speculative_->document_);
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
    for (const auto& binding : diff.material_bindings) {
        all_affected_assets.push_back(binding.key.mesh_asset);
    }
    const auto append_material_assets = [&all_affected_assets](
            const project::MaterialCatalog& catalog,
            render::MaterialId material) {
        for (const auto& [key, assignment] : catalog.assignments()) {
            if (assignment.material == material) all_affected_assets.push_back(key.mesh_asset);
        }
    };
    for (const auto& material : diff.material_sources) {
        append_material_assets(base_document_.material_catalog(), material.id);
        append_material_assets(speculative_->document_.material_catalog(), material.id);
    }
    for (const auto& texture : diff.texture_sources) {
        for (const project::MaterialCatalog* catalog : {
                 &base_document_.material_catalog(),
                 &speculative_->document_.material_catalog()}) {
            for (const auto& [material_id, material] : catalog->materials()) {
                if (material_uses_texture(material.definition, texture.id)) {
                    append_material_assets(*catalog, material_id);
                }
            }
        }
    }
    const auto append_assets_to_objects = [
            &all_affected_assets, &all_affected_objects](
            const project::ProjectDocument& document) {
        for (const auto& object : document.scene().objects_sorted()) {
            if (object.mesh_asset.has_value() && std::find(
                    all_affected_assets.begin(), all_affected_assets.end(),
                    *object.mesh_asset) != all_affected_assets.end()) {
                all_affected_objects.push_back(object.id);
            }
        }
    };
    append_assets_to_objects(base_document_);
    append_assets_to_objects(speculative_->document_);
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
    const bool changed = diff.has_changes();
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
        std::vector<scene::ObjectId> constraint_objects = constraint.object.has_value()
            ? std::vector<scene::ObjectId>{*constraint.object}
            : plan_.objective.scope_objects;
        std::vector<std::uint64_t> constraint_assets;
        if (constraint.object.has_value()) {
            const auto* object = base_document_.scene().find(*constraint.object);
            if (object != nullptr && object->mesh_asset.has_value()) {
                constraint_assets.push_back(*object->mesh_asset);
            }
        } else {
            for (const auto object_id : constraint_objects) {
                const auto* object = base_document_.scene().find(object_id);
                if (object != nullptr && object->mesh_asset.has_value()) {
                    constraint_assets.push_back(*object->mesh_asset);
                }
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
            for (const auto object_id : constraint_objects) {
                const auto before_transform =
                    base_document_.scene().world_transform(object_id);
                const auto after_transform =
                    speculative_->document_.scene().world_transform(object_id);
                if (!before_transform || !after_transform ||
                    !same_transform(before_transform.value(), after_transform.value())) {
                    passed = false;
                    break;
                }
            }
            evidence = passed
                ? (constraint.object.has_value()
                    ? "target world transform is unchanged"
                    : "all affected world transforms are unchanged")
                : "an affected world transform changed or could not be resolved";
        } else {
            double maximum_observed = 0.0;
            bool measurement_complete = true;
            std::size_t topology_elements_examined = 0U;
            for (const auto object_id : constraint_objects) {
                const auto* before_object = base_document_.scene().find(object_id);
                const auto* after_object = speculative_->document_.scene().find(object_id);
                if (before_object == nullptr || after_object == nullptr ||
                    before_object->mesh_asset != after_object->mesh_asset) {
                    passed = false;
                    measurement_complete = false;
                    continue;
                }
                if (!before_object->mesh_asset.has_value()) continue;
                const auto before_transform =
                    base_document_.scene().world_transform(object_id);
                const auto after_transform =
                    speculative_->document_.scene().world_transform(object_id);
                const auto before_mesh =
                    base_document_.meshes().find(*before_object->mesh_asset);
                const auto after_mesh = speculative_->document_.meshes().find(
                    *after_object->mesh_asset);
                if (!before_transform || !after_transform ||
                    before_mesh == base_document_.meshes().end() ||
                    after_mesh == speculative_->document_.meshes().end()) {
                    passed = false;
                    measurement_complete = false;
                    continue;
                }
                const std::size_t vertex_count = before_mesh->second.vertex_count();
                if (vertex_count != after_mesh->second.vertex_count()) {
                    passed = false;
                    measurement_complete = false;
                    continue;
                }
                if (topology_elements_examined > envelope_.max_topology_elements_examined ||
                    vertex_count > envelope_.max_topology_elements_examined -
                        topology_elements_examined) {
                    passed = false;
                    measurement_complete = false;
                    continue;
                }
                topology_elements_examined += vertex_count;
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
    view.ghost_recomputes = ghost_recompute_lineage_;
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

core::Result<void> AgentDemonstrationTrace::save(
    const std::filesystem::path& path) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            "demonstration trace destination must name a file"));
    }
    const auto encoded = serialize();
    if (!encoded) return core::Result<void>::failure(encoded.error());

    std::error_code error;
    const auto destination_status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect demonstration trace destination"));
    }
    if (std::filesystem::exists(destination_status)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration traces are immutable; choose a new destination path"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        const auto parent_status = std::filesystem::status(parent, error);
        if (error || !std::filesystem::is_directory(parent_status)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "demonstration trace destination directory does not exist"));
        }
    }

    auto lock_path = path;
    lock_path += ".lock";
    if (!std::filesystem::create_directory(lock_path, error)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration trace destination is already being written"));
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
                core::ErrorCode::io_error,
                "unable to create demonstration trace temporary file"));
        }
        output.write(encoded.value().data(),
            static_cast<std::streamsize>(encoded.value().size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary_path, error);
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to flush demonstration trace temporary file"));
        }
    }
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to publish immutable demonstration trace"));
    }
    return core::Result<void>::success();
}

core::Result<AgentDemonstrationTrace> AgentDemonstrationTrace::load(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<AgentDemonstrationTrace>::failure(invalid(
            "demonstration trace source must name a file"));
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 1U * 1024U * 1024U) {
        return core::Result<AgentDemonstrationTrace>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration trace is unavailable or exceeds its size bound"));
    }
    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input) {
        return core::Result<AgentDemonstrationTrace>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to open demonstration trace file"));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        return core::Result<AgentDemonstrationTrace>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to read complete demonstration trace"));
    }
    return AgentDemonstrationTrace::deserialize(bytes);
}

core::Result<void> AgentDemonstrationRecipe::save(
    const std::filesystem::path& path) const {
    const auto encoded = serialize();
    if (!encoded) return core::Result<void>::failure(encoded.error());
    return save_immutable_artifact(path, encoded.value(), "demonstration recipe");
}

core::Result<AgentDemonstrationRecipe> AgentDemonstrationRecipe::load(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<AgentDemonstrationRecipe>::failure(invalid(
            "demonstration recipe source must name a file"));
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 2U * 1024U * 1024U) {
        return core::Result<AgentDemonstrationRecipe>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "demonstration recipe is unavailable or exceeds its size bound"));
    }
    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input) {
        return core::Result<AgentDemonstrationRecipe>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to open demonstration recipe file"));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        return core::Result<AgentDemonstrationRecipe>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to read complete demonstration recipe"));
    }
    return AgentDemonstrationRecipe::deserialize(bytes);
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
    checkpoint.ghost_recomputes = ghost_recompute_lineage_;
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
    resumed.value().ghost_recompute_lineage_ = checkpoint.ghost_recomputes;
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
    std::set<scene::ObjectId> transform_write_roots;
    std::map<std::string, std::set<scene::ObjectId>> write_objects_by_step;
    std::map<std::string, std::set<std::uint64_t>> write_meshes_by_step;
    std::map<scene::ObjectId, std::set<std::string>> steps_by_object_dependency;
    std::map<std::uint64_t, std::set<std::string>> steps_by_mesh_dependency;
    std::map<project::MaterialRegionKey, std::set<std::string>>
        steps_by_material_region_dependency;
    std::set<std::uint64_t> read_mesh_assets;
    std::set<std::uint64_t> write_mesh_assets;
    std::set<project::MaterialRegionKey> read_material_regions;
    std::set<project::MaterialRegionKey> write_material_regions;
    std::set<std::string> material_source_dependent_steps;
    std::set<std::string> material_conflicting_steps;
    const auto add_blocker = [&analysis](std::string message) {
        if (analysis.blockers.size() < 512U &&
            std::find(analysis.blockers.begin(), analysis.blockers.end(), message) ==
                analysis.blockers.end()) {
            analysis.blockers.push_back(std::move(message));
        }
    };
    const auto add_read_ancestry = [
        &read_objects, &add_blocker, &steps_by_object_dependency](
        const project::ProjectDocument& document,
        scene::ObjectId object,
        std::optional<std::string_view> step_id) {
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
            if (step_id.has_value()) {
                steps_by_object_dependency[parent].insert(std::string(*step_id));
            }
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

    for (const auto& step : checkpoint.plan.steps) {
        read_objects.insert(step.target.object);
        steps_by_object_dependency[step.target.object].insert(step.id);
        const bool material_assignment = intent_starts_with(
            step.intent, "assign", "material");
        const auto material_intent = material_assignment
            ? parse_material_assignment_intent(step.intent) : std::nullopt;
        if (material_assignment) {
            material_source_dependent_steps.insert(step.id);
            if (!material_intent.has_value()) {
                add_blocker(
                    "checkpoint material assignment intent cannot be represented by a bounded region write set");
            }
            if (!std::holds_alternative<std::monostate>(step.target.component)) {
                add_blocker(
                    "checkpoint material assignment does not use an object target");
            }
        }
        const auto* target = base.scene().find(step.target.object);
        if (target == nullptr) {
            add_blocker("checkpoint step target " + std::to_string(step.target.object.value) +
                " is absent from its saved source");
            continue;
        }
        add_read_ancestry(base, step.target.object, step.id);
        if (!material_assignment &&
            std::holds_alternative<std::monostate>(step.target.component)) {
            transform_write_roots.insert(step.target.object);
            write_objects.insert(step.target.object);
            write_objects_by_step[step.id].insert(step.target.object);
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
                        read_objects.insert(object.id);
                        write_objects.insert(object.id);
                        write_objects_by_step[step.id].insert(object.id);
                        steps_by_object_dependency[object.id].insert(step.id);
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
            add_blocker("checkpoint mesh step target " + std::to_string(step.target.object.value) +
                " has no saved mesh binding");
            continue;
        }
        const std::uint64_t asset = *target->mesh_asset;
        read_mesh_assets.insert(asset);
        write_mesh_assets.insert(asset);
        write_meshes_by_step[step.id].insert(asset);
        steps_by_mesh_dependency[asset].insert(step.id);
        if (material_assignment && material_intent.has_value()) {
            const project::MaterialRegionKey key{asset, material_intent->region};
            read_material_regions.insert(key);
            write_material_regions.insert(key);
            steps_by_material_region_dependency[key].insert(step.id);
            const auto material = base.material_catalog().materials().find(
                material_intent->material);
            const auto resolved = base.material_catalog().resolve_material(
                material_intent->material, material_intent->variant);
            if (material == base.material_catalog().materials().end() || !resolved) {
                add_blocker(
                    "checkpoint material assignment source does not resolve in its saved catalog");
            }
            if (material_intent->variant.has_value()) {
                const auto variant = base.material_catalog().variants().find(
                    *material_intent->variant);
                if (variant == base.material_catalog().variants().end() ||
                    (!variant->second.permitted_mesh_assets.empty() &&
                        !std::binary_search(variant->second.permitted_mesh_assets.begin(),
                            variant->second.permitted_mesh_assets.end(), asset))) {
                    add_blocker(
                        "checkpoint material variant does not permit its saved mesh asset");
                }
            }
        }
        for (const auto& instance : base.scene().objects_sorted()) {
            if (instance.mesh_asset == target->mesh_asset) {
                read_objects.insert(instance.id);
                steps_by_object_dependency[instance.id].insert(step.id);
                add_read_ancestry(base, instance.id, step.id);
            }
        }
    }
    for (const auto& correction : checkpoint.ghost_corrections) {
        read_objects.insert(correction.object);
        add_read_ancestry(base, correction.object, std::nullopt);
        if (correction.kind == GhostCorrectionKind::object_transform) {
            transform_write_roots.insert(correction.object);
            write_objects.insert(correction.object);
            for (const auto& object : base.scene().objects_sorted()) {
                const scene::SceneObject* ancestor = &object;
                std::set<scene::ObjectId> visited;
                for (std::size_t depth = 0U; ancestor != nullptr &&
                     ancestor->parent.has_value(); ++depth) {
                    const scene::ObjectId parent = *ancestor->parent;
                    if (depth >= 4096U || !visited.insert(parent).second) {
                        add_blocker("checkpoint ghost-transform hierarchy is invalid");
                        break;
                    }
                    if (parent == correction.object) {
                        read_objects.insert(object.id);
                        write_objects.insert(object.id);
                        break;
                    }
                    ancestor = base.scene().find(parent);
                    if (ancestor == nullptr) {
                        add_blocker("checkpoint ghost-transform hierarchy has a missing parent");
                    }
                }
            }
            continue;
        }
        const auto* target = base.scene().find(correction.object);
        if (target == nullptr || !target->mesh_asset.has_value()) {
            add_blocker("checkpoint ghost mesh correction has no saved mesh binding");
            continue;
        }
        read_mesh_assets.insert(*target->mesh_asset);
        write_mesh_assets.insert(*target->mesh_asset);
        for (const auto& instance : base.scene().objects_sorted()) {
            if (instance.mesh_asset == target->mesh_asset) read_objects.insert(instance.id);
        }
    }
    for (const auto object : write_objects) add_read_ancestry(base, object, std::nullopt);

    if (!read_material_regions.empty()) {
        const auto base_material_sources = material_catalog_source_projection(
            base.material_catalog());
        const auto current_material_sources = material_catalog_source_projection(
            current.material_catalog());
        if (!base_material_sources || !current_material_sources) {
            add_blocker(
                "checkpoint material catalog dependencies could not be inspected safely");
            material_conflicting_steps.insert(
                material_source_dependent_steps.begin(),
                material_source_dependent_steps.end());
        } else if (base_material_sources.value() != current_material_sources.value()) {
            add_blocker(
                "material source definitions changed since the checkpoint source");
            material_conflicting_steps.insert(
                material_source_dependent_steps.begin(),
                material_source_dependent_steps.end());
        }
        for (const auto key : read_material_regions) {
            if (!same_material_assignment_at(
                    base.material_catalog(), current.material_catalog(), key)) {
                add_blocker(
                    "material region assignment changed since the checkpoint source");
                const auto steps = steps_by_material_region_dependency.find(key);
                if (steps != steps_by_material_region_dependency.end()) {
                    material_conflicting_steps.insert(
                        steps->second.begin(), steps->second.end());
                }
            }
        }
    }

    std::set<scene::ObjectId> object_ids;
    for (const auto& object : base.scene().objects_sorted()) object_ids.insert(object.id);
    for (const auto& object : current.scene().objects_sorted()) object_ids.insert(object.id);
    std::set<scene::ObjectId> changed_objects;
    const auto is_in_transform_write_subtree = [&transform_write_roots](
        const project::ProjectDocument& document, scene::ObjectId object) {
        std::set<scene::ObjectId> visited;
        const scene::SceneObject* current_object = document.scene().find(object);
        for (std::size_t depth = 0U; current_object != nullptr; ++depth) {
            if (transform_write_roots.contains(current_object->id)) return true;
            if (!current_object->parent.has_value()) return false;
            if (depth >= 4096U || !visited.insert(*current_object->parent).second) {
                return true;
            }
            current_object = document.scene().find(*current_object->parent);
        }
        return false;
    };
    for (const scene::ObjectId object : object_ids) {
        const auto* before = base.scene().find(object);
        const auto* after = current.scene().find(object);
        if (same_scene_object(before, after)) continue;
        changed_objects.insert(object);
        const bool read_conflict = read_objects.contains(object);
        const bool asset_membership_changed = before == nullptr || after == nullptr ||
            before->mesh_asset != after->mesh_asset;
        const bool shared_write_asset = asset_membership_changed &&
            ((before != nullptr && before->mesh_asset.has_value() &&
                write_mesh_assets.contains(*before->mesh_asset)) ||
             (after != nullptr && after->mesh_asset.has_value() &&
                write_mesh_assets.contains(*after->mesh_asset)));
        const bool write_conflict = write_objects.contains(object);
        const bool changed_transform_subtree =
            is_in_transform_write_subtree(base, object) ||
            is_in_transform_write_subtree(current, object);
        if (write_conflict || shared_write_asset ||
            changed_transform_subtree ||
            (read_conflict && (before == nullptr || after == nullptr))) {
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
            if (write_conflict) {
                add_blocker("write object " + std::to_string(object.value) +
                    " changed since the checkpoint source");
            } else if (changed_transform_subtree) {
                add_blocker("object " + std::to_string(object.value) +
                    " changed membership in a root-transform write subtree");
            } else if (read_conflict && (before == nullptr || after == nullptr)) {
                add_blocker("read object " + std::to_string(object.value) +
                    " was added or removed since the checkpoint source");
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
    std::set<std::uint64_t> changed_meshes;
    for (const std::uint64_t asset : mesh_ids) {
        const auto before = base.meshes().find(asset);
        const auto after = current.meshes().find(asset);
        const auto* before_mesh = before == base.meshes().end() ? nullptr : &before->second;
        const auto* after_mesh = after == current.meshes().end() ? nullptr : &after->second;
        if (same_mesh_state(before_mesh, after_mesh)) continue;
        changed_meshes.insert(asset);
        if (!read_mesh_assets.contains(asset)) continue;
        analysis.conflicting_mesh_assets.push_back(asset);
        if (write_mesh_assets.contains(asset) || before_mesh == nullptr || after_mesh == nullptr) {
            add_blocker("write mesh asset " + std::to_string(asset) +
                " changed since the checkpoint source");
        }
    }
    const bool semantic_sidecar_changed =
        base.condition_authoring() != current.condition_authoring() ||
        base.engineering_authoring() != current.engineering_authoring();
    if (semantic_sidecar_changed) {
        add_blocker("CONDITION or engineering sidecar changed; checkpoint has no field-level semantic read provenance");
    }

    // Identify the smallest plan branch whose source inputs changed. Read-only
    // changes invalidate those nodes and their dependents for replay; overlapping
    // write conflicts remain blockers.
    std::set<std::string> directly_conflicting_steps = material_conflicting_steps;
    const auto mark_steps = [&directly_conflicting_steps](
        const auto& dependencies, const auto& identity) {
        const auto found = dependencies.find(identity);
        if (found != dependencies.end()) {
            directly_conflicting_steps.insert(found->second.begin(), found->second.end());
        }
    };
    for (const auto object : changed_objects) {
        mark_steps(steps_by_object_dependency, object);
        const auto* before = base.scene().find(object);
        const auto* after = current.scene().find(object);
        if (before != nullptr && before->mesh_asset.has_value()) {
            mark_steps(steps_by_mesh_dependency, *before->mesh_asset);
        }
        if (after != nullptr && after->mesh_asset.has_value()) {
            mark_steps(steps_by_mesh_dependency, *after->mesh_asset);
        }
    }
    for (const auto asset : changed_meshes) {
        mark_steps(steps_by_mesh_dependency, asset);
    }
    const bool ghost_read_dependency_changed = !checkpoint.ghost_corrections.empty() &&
        (std::any_of(changed_objects.begin(), changed_objects.end(),
             [&read_objects](scene::ObjectId object) {
                 return read_objects.contains(object);
             }) ||
         std::any_of(changed_meshes.begin(), changed_meshes.end(),
             [&read_mesh_assets](std::uint64_t asset) {
                 return read_mesh_assets.contains(asset);
             }));
    if (ghost_read_dependency_changed) {
        for (const auto& step : checkpoint.plan.steps) {
            directly_conflicting_steps.insert(step.id);
        }
    }
    if (semantic_sidecar_changed) {
        for (const auto& step : checkpoint.plan.steps) {
            directly_conflicting_steps.insert(step.id);
        }
    }
    std::set<std::string> invalidated_steps = directly_conflicting_steps;
    bool added_dependent = true;
    while (added_dependent) {
        added_dependent = false;
        for (const auto& step : checkpoint.plan.steps) {
            if (invalidated_steps.contains(step.id)) continue;
            bool depends_on_invalidated = std::any_of(
                step.dependencies.begin(), step.dependencies.end(),
                [&invalidated_steps](const std::string& dependency) {
                    return invalidated_steps.contains(dependency);
                });
            if (!depends_on_invalidated) {
                for (const auto& invalidated_id : invalidated_steps) {
                    const auto object_writes = write_objects_by_step.find(invalidated_id);
                    if (object_writes != write_objects_by_step.end() &&
                        std::any_of(object_writes->second.begin(),
                            object_writes->second.end(),
                            [&steps_by_object_dependency, &step](scene::ObjectId object) {
                                const auto readers = steps_by_object_dependency.find(object);
                                return readers != steps_by_object_dependency.end() &&
                                    readers->second.contains(step.id);
                            })) {
                        depends_on_invalidated = true;
                        break;
                    }
                    const auto mesh_writes = write_meshes_by_step.find(invalidated_id);
                    if (mesh_writes != write_meshes_by_step.end() &&
                        std::any_of(mesh_writes->second.begin(), mesh_writes->second.end(),
                            [&steps_by_mesh_dependency, &step](std::uint64_t asset) {
                                const auto readers = steps_by_mesh_dependency.find(asset);
                                return readers != steps_by_mesh_dependency.end() &&
                                    readers->second.contains(step.id);
                            })) {
                        depends_on_invalidated = true;
                        break;
                    }
                }
            }
            if (depends_on_invalidated) {
                invalidated_steps.insert(step.id);
                added_dependent = true;
            }
        }
    }
    const auto scheduled_steps = checkpoint.plan.schedule_order();
    if (!scheduled_steps) {
        add_blocker("checkpoint plan dependency graph cannot be scheduled for rebase analysis");
    } else {
        for (const auto& step_id : scheduled_steps.value()) {
            if (directly_conflicting_steps.contains(step_id)) {
                analysis.directly_conflicting_step_ids.push_back(step_id);
            }
            if (invalidated_steps.contains(step_id)) {
                analysis.invalidated_step_ids.push_back(step_id);
            }
        }
    }

    analysis.read_objects.assign(read_objects.begin(), read_objects.end());
    analysis.write_objects.assign(write_objects.begin(), write_objects.end());
    analysis.read_mesh_assets.assign(read_mesh_assets.begin(), read_mesh_assets.end());
    analysis.write_mesh_assets.assign(write_mesh_assets.begin(), write_mesh_assets.end());
    analysis.read_material_regions.assign(
        read_material_regions.begin(), read_material_regions.end());
    analysis.write_material_regions.assign(
        write_material_regions.begin(), write_material_regions.end());
    std::sort(analysis.conflicting_objects.begin(), analysis.conflicting_objects.end());
    analysis.conflicting_objects.erase(std::unique(
        analysis.conflicting_objects.begin(), analysis.conflicting_objects.end()),
        analysis.conflicting_objects.end());
    std::sort(analysis.conflicting_mesh_assets.begin(), analysis.conflicting_mesh_assets.end());
    analysis.conflicting_mesh_assets.erase(std::unique(
        analysis.conflicting_mesh_assets.begin(), analysis.conflicting_mesh_assets.end()),
        analysis.conflicting_mesh_assets.end());
    analysis.read_set_digest = rebase_set_digest(
        "read", analysis.read_objects, analysis.read_mesh_assets,
        analysis.read_material_regions);
    analysis.write_set_digest = rebase_set_digest(
        "write", analysis.write_objects, analysis.write_mesh_assets,
        analysis.write_material_regions);
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
    rebased.value().ghost_recompute_lineage_ = checkpoint.ghost_recomputes;
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
    receipt.read_material_regions = analysis.value().read_material_regions;
    receipt.write_material_regions = analysis.value().write_material_regions;

    // A review checkpoint whose dependency graph is untouched can reuse its
    // verified artifact. First replay it against the exact historical source;
    // then three-way merge only its declared writes onto the current source.
    // Changed plan inputs continue through the affected-branch replay below.
    if (checkpoint.status == TaskStatus::review &&
        analysis.value().invalidated_step_ids.empty()) {
        auto historical_base = project::ProjectDocument::deserialize(
            checkpoint.source_project);
        if (!historical_base) {
            return core::Result<AgentTask>::failure(historical_base.error());
        }
        auto merge_base = historical_base.value();
        auto historical_source = std::unique_ptr<application::ApplicationSession>(
            new application::ApplicationSession(
                source, application::ApplicationSession::SpeculativeCopyTag{}));
        if (auto installed = historical_source->install_agent_checkpoint_source(
                std::move(historical_base.value())); !installed) {
            return core::Result<AgentTask>::failure(installed.error());
        }
        auto historical_speculative = std::unique_ptr<application::ApplicationSession>(
            new application::ApplicationSession(
                *historical_source, application::ApplicationSession::SpeculativeCopyTag{}));
        AgentTask historical_task(
            *historical_source, checkpoint.plan, checkpoint.envelope,
            std::move(historical_speculative), historical_source->snapshot(),
            checkpoint.repair_lineage, checkpoint.rebase_lineage);
        historical_task.ghost_corrections_ = checkpoint.ghost_corrections;
        historical_task.ghost_recompute_lineage_ = checkpoint.ghost_recomputes;
        historical_task.replay_ghost_corrections_ =
            !historical_task.ghost_corrections_.empty();
        const auto historical_run = historical_task.run();
        if (!historical_run || historical_task.status_ != TaskStatus::review ||
            digest_text(historical_task.speculative_->document_.serialize()) !=
                checkpoint.speculative_digest ||
            historical_task.build_diff().digest != checkpoint.diff_digest) {
            return core::Result<AgentTask>::failure(stale(
                "checkpoint did not reproduce its saved review branch on the historical source"));
        }

        auto merged = source.merge_agent_rebase_document(
            merge_base, saved_speculative.value(),
            analysis.value().read_objects, analysis.value().read_mesh_assets,
            analysis.value().write_objects, analysis.value().write_mesh_assets,
            analysis.value().read_material_regions,
            analysis.value().write_material_regions);
        if (!merged) return core::Result<AgentTask>::failure(merged.error());
        for (const auto& progress : historical_task.plan_progress_) {
            if (progress.status == StepStatus::passed) {
                receipt.reused_step_ids.push_back(progress.step_id);
            }
        }
        std::sort(receipt.reused_step_ids.begin(), receipt.reused_step_ids.end());
        if (auto valid = receipt.validate(); !valid) {
            return core::Result<AgentTask>::failure(valid.error());
        }
        const auto installed = rebased.value().speculative_->install_agent_rebase_branch(
            merged.value(), analysis.value().current_revision, checkpoint.plan.id,
            checkpoint.plan_context_digest.hex(), checkpoint.source_digest.hex());
        if (!installed) {
            return core::Result<AgentTask>::failure(installed.error());
        }

        rebased.value().receipts_ = std::move(historical_task.receipts_);
        rebased.value().plan_progress_ = std::move(historical_task.plan_progress_);
        rebased.value().touched_objects_ = std::move(historical_task.touched_objects_);
        rebased.value().transform_distance_used_meters_ =
            historical_task.transform_distance_used_meters_;
        rebased.value().query_budget_ = historical_task.query_budget_;
        rebased.value().ghost_correction_replay_failed_ =
            historical_task.ghost_correction_replay_failed_;
        rebased.value().replay_ghost_corrections_ =
            historical_task.replay_ghost_corrections_;
        rebased.value().diagnostic_ = historical_task.diagnostic_;
        rebased.value().rebases_.push_back(std::move(receipt));
        rebased.value().status_ = TaskStatus::review;
        rebased.value().gates_ = rebased.value().evaluate_gates();
        const bool required_gates_pass = std::all_of(
            rebased.value().gates_.begin(), rebased.value().gates_.end(),
            [](const AcceptanceGateResult& gate) {
                return !gate.required || gate.passed;
            });
        if (!required_gates_pass) {
            rebased.value().status_ = TaskStatus::blocked;
            rebased.value().diagnostic_ = invalid_state(
                "rebased checkpoint reuse failed a required acceptance gate");
        }
        rebased.value().cached_snapshot_.reset();
        return rebased;
    }

    if (checkpoint.status == TaskStatus::review &&
        !analysis.value().invalidated_step_ids.empty() &&
        checkpoint.ghost_corrections.empty()) {
        auto historical_base = project::ProjectDocument::deserialize(
            checkpoint.source_project);
        if (!historical_base) {
            return core::Result<AgentTask>::failure(historical_base.error());
        }
        auto merge_base = historical_base.value();
        auto historical_source = std::unique_ptr<application::ApplicationSession>(
            new application::ApplicationSession(
                source, application::ApplicationSession::SpeculativeCopyTag{}));
        if (auto installed = historical_source->install_agent_checkpoint_source(
                std::move(historical_base.value())); !installed) {
            return core::Result<AgentTask>::failure(installed.error());
        }
        auto historical_speculative = std::unique_ptr<application::ApplicationSession>(
            new application::ApplicationSession(
                *historical_source, application::ApplicationSession::SpeculativeCopyTag{}));
        AgentTask historical_task(
            *historical_source, checkpoint.plan, checkpoint.envelope,
            std::move(historical_speculative), historical_source->snapshot(),
            checkpoint.repair_lineage, checkpoint.rebase_lineage);
        const auto historical_run = historical_task.run();
        if (!historical_run || historical_task.status_ != TaskStatus::review ||
            digest_text(historical_task.speculative_->document_.serialize()) !=
                checkpoint.speculative_digest ||
            historical_task.build_diff().digest != checkpoint.diff_digest) {
            return core::Result<AgentTask>::failure(stale(
                "checkpoint did not reproduce its saved review branch on the historical source"));
        }

        std::set<std::string> invalidated_ids(
            analysis.value().invalidated_step_ids.begin(),
            analysis.value().invalidated_step_ids.end());
        std::map<scene::ObjectId, std::string> object_writers;
        std::map<std::uint64_t, std::string> mesh_writers;
        const bool has_material_assignment = std::any_of(
            checkpoint.plan.steps.begin(), checkpoint.plan.steps.end(),
            [](const AgentPlanStep& step) {
                return intent_starts_with(step.intent, "assign", "material");
            });
        bool can_selectively_replay = !has_material_assignment;
        for (const auto& step : checkpoint.plan.steps) {
            if (!can_selectively_replay) break;
            if (std::holds_alternative<std::monostate>(step.target.component)) {
                for (const auto& object : merge_base.scene().objects_sorted()) {
                    const scene::SceneObject* ancestor = &object;
                    std::set<scene::ObjectId> visited;
                    bool affected = object.id == step.target.object;
                    for (std::size_t depth = 0U; !affected && ancestor != nullptr &&
                         ancestor->parent.has_value(); ++depth) {
                        const scene::ObjectId parent = *ancestor->parent;
                        if (depth >= 4096U || !visited.insert(parent).second) {
                            can_selectively_replay = false;
                            break;
                        }
                        if (parent == step.target.object) {
                            affected = true;
                            break;
                        }
                        ancestor = merge_base.scene().find(parent);
                    }
                    if (!can_selectively_replay) break;
                    if (affected &&
                        !object_writers.emplace(object.id, step.id).second) {
                        can_selectively_replay = false;
                        break;
                    }
                }
            } else {
                const auto* target = merge_base.scene().find(step.target.object);
                if (target == nullptr || !target->mesh_asset.has_value() ||
                    !mesh_writers.emplace(*target->mesh_asset, step.id).second) {
                    can_selectively_replay = false;
                }
            }
            if (!can_selectively_replay) break;
        }

        std::set<std::string> reused_ids;
        if (can_selectively_replay) {
            const auto schedule = checkpoint.plan.schedule_order();
            if (!schedule) {
                can_selectively_replay = false;
            }
            if (schedule) {
                for (const auto& step_id : schedule.value()) {
                    if (!invalidated_ids.contains(step_id)) reused_ids.insert(step_id);
                }
            }
            if (can_selectively_replay) {
                can_selectively_replay = !reused_ids.empty() && std::all_of(
                    reused_ids.begin(), reused_ids.end(),
                    [&historical_task](const auto& step_id) {
                        return std::any_of(historical_task.receipts_.begin(),
                            historical_task.receipts_.end(),
                            [&step_id](const StepReceipt& receipt) {
                                return receipt.step_id == step_id &&
                                    receipt.status == StepStatus::passed;
                            });
                    });
            }
        }
        if (can_selectively_replay) {
            receipt.reused_step_ids.assign(reused_ids.begin(), reused_ids.end());
            if (auto valid = receipt.validate(); !valid) {
                return core::Result<AgentTask>::failure(valid.error());
            }
            rebased.value().rebases_.push_back(std::move(receipt));
            rebased.value().rebase_source_document_.emplace(std::move(merge_base));
            rebased.value().rebase_speculative_document_.emplace(
                historical_task.speculative_->document_);
            rebased.value().rebase_reused_step_ids_ = std::move(reused_ids);
            for (const auto& historical_receipt : historical_task.receipts_) {
                if (rebased.value().rebase_reused_step_ids_.contains(
                        historical_receipt.step_id)) {
                    rebased.value().rebase_reused_step_receipts_.emplace(
                        historical_receipt.step_id, historical_receipt);
                }
            }
            for (const auto& [step_id, distance] :
                 historical_task.step_transform_distance_meters_) {
                if (rebased.value().rebase_reused_step_ids_.contains(step_id)) {
                    rebased.value().step_transform_distance_meters_.emplace(
                        step_id, distance);
                }
            }
            rebased.value().touched_objects_ = historical_task.touched_objects_;
            rebased.value().query_budget_ = historical_task.query_budget_;
            const auto replayed = rebased.value().run();
            if (!replayed) return core::Result<AgentTask>::failure(replayed.error());
            rebased.value().cached_snapshot_.reset();
            return rebased;
        }
    }

    if (auto valid = receipt.validate(); !valid) {
        return core::Result<AgentTask>::failure(valid.error());
    }
    rebased.value().rebases_.push_back(std::move(receipt));
    const auto replayed = rebased.value().run();
    if (!replayed) return core::Result<AgentTask>::failure(replayed.error());
    rebased.value().cached_snapshot_.reset();

    if (checkpoint.status != TaskStatus::prepared &&
        analysis.value().invalidated_step_ids.empty()) {
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
        for (const auto key : analysis.value().write_material_regions) {
            if (!same_material_assignment_at(
                    saved_speculative.value().material_catalog(),
                    rebased.value().speculative_->document_.material_catalog(), key)) {
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

core::Result<void> HumanApplicationAccess::correct_agent_ghost_vertex_position(
    ::carto::agent::AgentTask& task,
    scene::ObjectId object,
    geometry::VertexId vertex,
    core::Vec3d position,
    std::string justification) {
    return task.correct_ghost_vertex_position(
        object, vertex, position, std::move(justification));
}

core::Result<void> HumanApplicationAccess::correct_agent_ghost_face(
    ::carto::agent::AgentTask& task,
    scene::ObjectId object,
    geometry::FaceId face,
    ::carto::agent::GhostCorrectionKind kind,
    double distance,
    std::string justification) {
    return task.correct_ghost_face(
        object, face, kind, distance, std::move(justification));
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
