#include <carto/ai/ai.hpp>
#include <carto/application/application.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

carto::ai::Context context_for(const carto::application::ApplicationSession& session) {
    const auto authoring = session.authoring_context();
    REQUIRE(authoring);
    const auto snapshot = session.snapshot();
    return carto::ai::Context{
        snapshot.project_revision,
        authoring.value(),
        carto::ai::build_operation_ontology(snapshot.tools)};
}

void ontology_is_derived_from_native_tools() {
    carto::application::ApplicationSession session;
    const auto context = context_for(session);
    REQUIRE(context.validate());
    REQUIRE(context.operations.size() >= 10U);

    const auto extrude = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.extrude-face"; });
    const auto inset = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.inset-face"; });
    const auto translate = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "object.translate"; });
    const auto material = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "material.assign-region"; });
    const auto lightmap_uv = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "uv.generate-lightmap"; });
    const auto poke = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.poke-face"; });
    const auto delete_face = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.remove-face"; });
    const auto merge_vertices = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.merge-vertices"; });
    const auto dissolve_edge = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.dissolve-edge"; });
    const auto tri_to_quad = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.tri-to-quad"; });
    const auto slide_vertex = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.slide-vertex"; });
    const auto bevel_edge = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.bevel-edge"; });
    const auto bevel_vertex = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.bevel-vertex"; });
    const auto loop_cut = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.loop-cut"; });
    const auto knife_cut = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.knife-cut"; });
    REQUIRE(extrude != context.operations.end() && extrude->preview_supported);
    REQUIRE(inset != context.operations.end() && inset->preview_supported);
    REQUIRE(translate != context.operations.end() && translate->preview_supported);
    REQUIRE(!translate->auto_approvable);
    REQUIRE(translate->effects == carto::ai::effect_mask(
        carto::ai::CapabilityEffect::transform));
    REQUIRE(translate->writes == std::vector<carto::ai::CapabilityWrite>{
        carto::ai::CapabilityWrite::object_transform});
    REQUIRE(translate->validators.size() == 5U);
    REQUIRE(translate->validators[1] == carto::ai::CapabilityValidator::root_object);
    REQUIRE(translate->validators[2] == carto::ai::CapabilityValidator::unlocked_subtree);
    REQUIRE(material != context.operations.end() && material->preview_supported);
    REQUIRE(material->provider_requestable && !material->auto_approvable);
    REQUIRE(material->effects == carto::ai::effect_mask(
        carto::ai::CapabilityEffect::material));
    REQUIRE(material->writes == std::vector<carto::ai::CapabilityWrite>{
        carto::ai::CapabilityWrite::material_region_assignment});
    REQUIRE(material->parameter_schema.size() == 3U);
    REQUIRE(material->parameter_schema.front().type ==
        carto::ai::ParameterType::unsigned_identity);
    REQUIRE(!material->parameter_schema.back().required);
    REQUIRE(lightmap_uv != context.operations.end() && lightmap_uv->preview_supported);
    REQUIRE(lightmap_uv->provider_requestable && !lightmap_uv->auto_approvable);
    REQUIRE(lightmap_uv->preview_kind ==
        carto::editor::PreviewKind::generate_unique_lightmap_uv);
    REQUIRE(lightmap_uv->effects == carto::ai::effect_mask(
        carto::ai::CapabilityEffect::uv));
    REQUIRE(lightmap_uv->writes == std::vector<carto::ai::CapabilityWrite>{
        carto::ai::CapabilityWrite::uv_set});
    REQUIRE(lightmap_uv->validators.back() ==
        carto::ai::CapabilityValidator::operation_preconditions);
    REQUIRE(std::find(lightmap_uv->validators.begin(), lightmap_uv->validators.end(),
        carto::ai::CapabilityValidator::lightmap_uv_generation_valid) !=
        lightmap_uv->validators.end());
    REQUIRE(lightmap_uv->precondition.find("shared mesh") != std::string::npos);
    REQUIRE(poke != context.operations.end() && poke->preview_supported);
    REQUIRE(poke->provider_requestable);
    REQUIRE(!poke->auto_approvable);
    REQUIRE(poke->preview_kind == carto::editor::PreviewKind::poke_face);
    REQUIRE(poke->parameter_schema.empty());
    REQUIRE(delete_face != context.operations.end() && !delete_face->preview_supported);
    REQUIRE(merge_vertices != context.operations.end() && !merge_vertices->preview_supported);
    REQUIRE(dissolve_edge != context.operations.end() && !dissolve_edge->preview_supported);
    REQUIRE(tri_to_quad != context.operations.end() && !tri_to_quad->preview_supported);
    REQUIRE(slide_vertex != context.operations.end() && !slide_vertex->preview_supported);
    REQUIRE(bevel_edge != context.operations.end() && !bevel_edge->preview_supported);
    REQUIRE(bevel_vertex != context.operations.end() && !bevel_vertex->preview_supported);
    REQUIRE(loop_cut != context.operations.end());
    REQUIRE(loop_cut->kernel_operation == "unclassified");
    REQUIRE(!loop_cut->contract_complete);
    REQUIRE(!loop_cut->preview_supported);
    REQUIRE(!loop_cut->preview_kind.has_value());
    REQUIRE(!loop_cut->provider_requestable);
    REQUIRE(!loop_cut->auto_approvable);
    REQUIRE(loop_cut->effects == 0U);
    REQUIRE(loop_cut->targets.empty());
    REQUIRE(loop_cut->reads.empty());
    REQUIRE(loop_cut->writes.empty());
    REQUIRE(loop_cut->invalidates.empty());
    REQUIRE(loop_cut->validators.empty());
    REQUIRE(loop_cut->risk == carto::ai::CapabilityRisk::unknown);
    REQUIRE(knife_cut != context.operations.end());
    REQUIRE(knife_cut->kernel_operation == "unclassified");
    REQUIRE(!knife_cut->contract_complete && !knife_cut->preview_supported);
    REQUIRE(!knife_cut->provider_requestable && !knife_cut->auto_approvable);
    REQUIRE(knife_cut->effects == 0U && knife_cut->targets.empty());
    REQUIRE(knife_cut->reads.empty() && knife_cut->writes.empty());
    REQUIRE(knife_cut->invalidates.empty() && knife_cut->validators.empty());
    REQUIRE(knife_cut->risk == carto::ai::CapabilityRisk::unknown);
    REQUIRE(extrude->kernel_operation == "geometry.EditableMesh.extrude_face");
    REQUIRE(inset->kernel_operation == "geometry.EditableMesh.inset_face");
    REQUIRE(inset->precondition.find("strictly convex") != std::string::npos);
    REQUIRE(inset->precondition.find("planar") != std::string::npos);
    REQUIRE(delete_face->kernel_operation == "geometry.EditableMesh.delete_face");
    REQUIRE(merge_vertices->kernel_operation == "geometry.EditableMesh.merge_vertices");
    REQUIRE(merge_vertices->precondition.find("exactly two") != std::string::npos);
    REQUIRE(dissolve_edge->kernel_operation == "geometry.EditableMesh.dissolve_edge");
    REQUIRE(dissolve_edge->precondition.find("coplanar") != std::string::npos);
    REQUIRE(dissolve_edge->precondition.find("future preview") != std::string::npos);
    REQUIRE(tri_to_quad->kernel_operation == "geometry.EditableMesh.tri_to_quad");
    REQUIRE(tri_to_quad->precondition.find("exactly two") != std::string::npos);
    REQUIRE(tri_to_quad->precondition.find("future preview") != std::string::npos);
    REQUIRE(slide_vertex->kernel_operation == "geometry.EditableMesh.slide_vertex");
    REQUIRE(slide_vertex->precondition.find("strictly between") != std::string::npos);
    REQUIRE(poke->kernel_operation == "geometry.EditableMesh.poke_face");
    REQUIRE(poke->precondition.find("strictly convex") != std::string::npos);
    REQUIRE(poke->precondition.find("centroid strictly inside") != std::string::npos);
    REQUIRE(bevel_edge->kernel_operation == "geometry.EditableMesh.bevel_edge");
    REQUIRE(bevel_edge->precondition.find("segments must equal one") != std::string::npos);
    REQUIRE(!bevel_edge->auto_approvable);
    REQUIRE(bevel_vertex->kernel_operation == "geometry.EditableMesh.bevel_vertex");
    REQUIRE(bevel_vertex->precondition.find("triangular manifold fan") != std::string::npos);
    REQUIRE(bevel_vertex->precondition.find("3 to 64") != std::string::npos);
    REQUIRE(bevel_vertex->precondition.find("simple, planar, and convex") != std::string::npos);
    REQUIRE(bevel_vertex->precondition.find("segments must equal one") != std::string::npos);
    REQUIRE(!bevel_vertex->auto_approvable);
    REQUIRE(std::all_of(context.operations.begin(), context.operations.end(),
        [](const auto& operation) {
            if (operation.id == "mesh.loop-cut" ||
                operation.id == "mesh.knife-cut" ||
                operation.id == "mesh.set-face-normal" ||
                operation.id == "mesh.set-edge-sharp") {
                return !operation.contract_complete && operation.effects == 0U &&
                    operation.targets.empty() && operation.reads.empty() &&
                    operation.writes.empty() && operation.validators.empty();
            }
            return operation.contract_complete && operation.effects != 0U &&
                !operation.targets.empty() && !operation.reads.empty() &&
                !operation.writes.empty() && !operation.validators.empty();
        }));
    REQUIRE(std::count_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.provider_requestable; }) == 7);
    REQUIRE(inset->parameter_schema.size() == 1U);
    REQUIRE(inset->parameter_schema.front().type == carto::ai::ParameterType::length);
    REQUIRE(inset->parameter_schema.front().unit == carto::ai::ParameterUnit::world_length);
    REQUIRE(!inset->parameter_schema.front().minimum_inclusive);
}

void ontology_json_preserves_kernel_and_approval_contract() {
    carto::application::ApplicationSession session;
    const auto context = context_for(session);
    const auto json = carto::ai::context_json(context);
    REQUIRE(json.find("geometry.EditableMesh.inset_face") != std::string::npos);
    REQUIRE(json.find("strictly convex") != std::string::npos);
    REQUIRE(json.find("\"auto_approvable\":true") != std::string::npos);
    REQUIRE(json.find("\"preview_kind\":\"inset_face\"") != std::string::npos);
    REQUIRE(json.find("\"preview_kind\":\"poke_face\"") != std::string::npos);
    REQUIRE(json.find("project.MaterialCatalog.assign_region") != std::string::npos);
    REQUIRE(json.find("production.UniqueLightmapUv.generate") != std::string::npos);
    REQUIRE(json.find("\"preview_kind\":\"generate_unique_lightmap_uv\"") !=
        std::string::npos);
    REQUIRE(json.find("\"effects\":[\"material\"]") != std::string::npos);
    REQUIRE(json.find("unsigned_identity") != std::string::npos);
    REQUIRE(json.find("\"parameter_schema\":[{\"name\":\"distance\"") !=
        std::string::npos);
    REQUIRE(json.find("\"effects\":[\"geometry\",\"topology\"]") !=
        std::string::npos);
    REQUIRE(json.find("\"invalidates\":[\"render_mesh\"") != std::string::npos);
}

void capability_projection_filters_by_preview_and_work_envelope() {
    carto::application::ApplicationSession session;
    const auto snapshot = session.snapshot();
    const auto registry = carto::ai::build_operation_ontology(snapshot.tools);
    const auto custom_normal = std::find_if(registry.begin(), registry.end(),
        [](const auto& operation) { return operation.id == "mesh.set-face-normal"; });
    const auto sharp_edge = std::find_if(registry.begin(), registry.end(),
        [](const auto& operation) { return operation.id == "mesh.set-edge-sharp"; });
    REQUIRE(custom_normal != registry.end());
    REQUIRE(sharp_edge != registry.end());
    REQUIRE(custom_normal->kernel_operation == "unclassified");
    REQUIRE(!custom_normal->contract_complete);
    REQUIRE(!custom_normal->preview_supported);
    REQUIRE(!custom_normal->provider_requestable);
    REQUIRE(!custom_normal->auto_approvable);
    REQUIRE(sharp_edge->kernel_operation == "unclassified");
    REQUIRE(!sharp_edge->contract_complete);
    REQUIRE(!sharp_edge->preview_supported);
    REQUIRE(!sharp_edge->provider_requestable);
    REQUIRE(!sharp_edge->auto_approvable);
    const auto geometry_only = carto::ai::project_capabilities(
        registry, carto::ai::effect_mask(carto::ai::CapabilityEffect::geometry));
    REQUIRE(geometry_only.size() == 1U);
    REQUIRE(geometry_only.front().id == "mesh.set-vertex-position");
    REQUIRE(geometry_only.front().provider_requestable);
    REQUIRE(carto::ai::project_capabilities(registry, 0U).empty());
    REQUIRE(carto::ai::project_capabilities(registry, 0x80000000U).empty());
    const auto transforms = carto::ai::project_capabilities(
        registry, carto::ai::effect_mask(carto::ai::CapabilityEffect::transform));
    REQUIRE(transforms.size() == 1U);
    REQUIRE(transforms.front().id == "object.translate");
    const auto material_only = carto::ai::project_capabilities(
        registry, carto::ai::effect_mask(carto::ai::CapabilityEffect::material));
    REQUIRE(material_only.size() == 1U);
    REQUIRE(material_only.front().id == "material.assign-region");
    const auto uv_only = carto::ai::project_capabilities(
        registry, carto::ai::effect_mask(carto::ai::CapabilityEffect::uv));
    REQUIRE(uv_only.size() == 1U);
    REQUIRE(uv_only.front().id == "uv.generate-lightmap");
    const auto without_uv = carto::ai::project_capabilities(
        registry, carto::ai::effect_mask(carto::ai::CapabilityEffect::material));
    REQUIRE(std::none_of(without_uv.begin(), without_uv.end(),
        [](const auto& capability) { return capability.effects ==
            carto::ai::effect_mask(carto::ai::CapabilityEffect::uv); }));
    const auto geometry_and_material = carto::ai::project_capabilities(
        registry, carto::ai::effect_mask(carto::ai::CapabilityEffect::geometry) |
            carto::ai::effect_mask(carto::ai::CapabilityEffect::material));
    REQUIRE(geometry_and_material.size() == 2U);
    REQUIRE(std::none_of(geometry_and_material.begin(), geometry_and_material.end(),
        [](const auto& capability) { return capability.id == "object.translate"; }));

    auto invalid = geometry_only.front();
    invalid.parameter_schema.front().minimum = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!invalid.validate());
    invalid = geometry_only.front();
    invalid.parameter_schema.front().name = "wrong-name";
    REQUIRE(!invalid.validate());
    const auto topology_edit = std::find_if(registry.begin(), registry.end(), [](const auto& op) {
        return op.id == "mesh.inset-face";
    });
    REQUIRE(topology_edit != registry.end());
    invalid = *topology_edit;
    invalid.parameter_schema.clear();
    REQUIRE(!invalid.validate());
    invalid = geometry_only.front();
    invalid.effects |= 0x80000000U;
    REQUIRE(!invalid.validate());
}

void ontology_fails_closed_on_tool_kernel_contract_drift() {
    carto::application::ApplicationSession session;
    auto tools = session.snapshot().tools;
    const auto extrude = std::find_if(tools.begin(), tools.end(), [](const auto& tool) {
        return tool.id == "mesh.extrude-face";
    });
    REQUIRE(extrude != tools.end());
    extrude->kernel = carto::editor::ToolKernelKind::unclassified;

    const auto ontology = carto::ai::build_operation_ontology(tools);
    const auto operation = std::find_if(ontology.begin(), ontology.end(), [](const auto& value) {
        return value.id == "mesh.extrude-face";
    });
    REQUIRE(operation != ontology.end());
    REQUIRE(operation->kernel_operation == "unclassified");
    REQUIRE(!operation->preview_supported);
    REQUIRE(!operation->auto_approvable);
    REQUIRE(!operation->contract_complete);
    REQUIRE(!operation->provider_requestable);
    REQUIRE(operation->precondition.find("do not match") != std::string::npos);
    const auto projected = carto::ai::project_capabilities(ontology,
        carto::ai::effect_mask(carto::ai::CapabilityEffect::geometry) |
        carto::ai::effect_mask(carto::ai::CapabilityEffect::topology));
    REQUIRE(projected.size() == 3U);
    REQUIRE(std::none_of(projected.begin(), projected.end(), [](const auto& capability) {
        return capability.id == "mesh.extrude-face";
    }));
    REQUIRE(std::any_of(projected.begin(), projected.end(), [](const auto& capability) {
        return capability.id == "mesh.poke-face";
    }));
}

void native_planner_compiles_a_face_intent_without_mutating_state() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, face}));

    const auto before = session.snapshot();
    auto context = context_for(session);
    carto::ai::NativePlanner planner;
    const auto proposal = planner.propose("extrude face by 0.25", context);
    REQUIRE(proposal);
    REQUIRE(proposal.value().tool_id == "mesh.extrude-face");
    REQUIRE(proposal.value().preview_kind == carto::editor::PreviewKind::extrude_face);
    REQUIRE(proposal.value().parameters.distance.has_value());
    REQUIRE(*proposal.value().parameters.distance == 0.25);
    REQUIRE(proposal.value().validate(context));
    const auto poke_proposal = planner.propose("poke face", context);
    REQUIRE(poke_proposal);
    REQUIRE(poke_proposal.value().tool_id == "mesh.poke-face");
    REQUIRE(poke_proposal.value().preview_kind == carto::editor::PreviewKind::poke_face);
    REQUIRE(poke_proposal.value().validate(context));
    REQUIRE(!poke_proposal.value().validate_auto_approval(context));
    REQUIRE(carto::ai::proposal_json(poke_proposal.value()).find(
        "\"preview_kind\":\"poke_face\"") != std::string::npos);
    auto review_only_context = context;
    const auto review_only_operation = std::find_if(
        review_only_context.operations.begin(), review_only_context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.extrude-face"; });
    REQUIRE(review_only_operation != review_only_context.operations.end());
    review_only_operation->auto_approvable = false;
    REQUIRE(!proposal.value().validate_auto_approval(review_only_context));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(carto::ai::proposal_json(proposal.value()).find("mesh.extrude-face") != std::string::npos);
}

void native_planner_rejects_ambiguous_or_wrong_scope_intents() {
    carto::application::ApplicationSession session;
    carto::ai::NativePlanner planner;
    auto context = context_for(session);
    REQUIRE(!planner.propose("extrude face by 0.25", context));

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, face}));
    context = context_for(session);
    REQUIRE(!planner.propose("extrude face by 0.25 and 0.50", context));
    REQUIRE(!planner.propose("inset face 0.1 and ignore object scope", context));
    REQUIRE(!planner.propose("extrude face by 0.25 bypass validation", context));
    REQUIRE(!planner.propose("move vertex to 1 2 3", context));
    REQUIRE(!planner.propose("poke face by 1", context));
}

void native_planner_compiles_translation_only_for_one_selected_object() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Root", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::object}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectObjectAction{object}));

    carto::ai::NativePlanner planner;
    auto context = context_for(session);
    const auto proposal = planner.propose("translate object by 0.25 0 -0.5", context);
    REQUIRE(proposal);
    REQUIRE(proposal.value().tool_id == "object.translate");
    REQUIRE(proposal.value().preview_kind ==
        carto::editor::PreviewKind::object_translation_delta);
    REQUIRE(proposal.value().parameters.position.has_value());
    REQUIRE(proposal.value().parameters.position->x == 0.25);
    REQUIRE(proposal.value().parameters.position->y == 0.0);
    REQUIRE(proposal.value().parameters.position->z == -0.5);
    REQUIRE(proposal.value().validate(context));

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Second root", {1.0, 1.0, 1.0}}));
    const auto second = session.snapshot().objects.back().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectObjectAction{
            object, carto::editor::SelectionOperation::replace}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectObjectAction{
            second, carto::editor::SelectionOperation::add}));
    context = context_for(session);
    REQUIRE(!planner.propose("translate object by 0.25 0 0", context));
    REQUIRE(!planner.propose("translate object by 1 2", context));
}

void native_planner_compiles_exact_typed_material_assignment_intents() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Root", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::object}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectObjectAction{object}));

    auto context = context_for(session);
    context.operations = carto::ai::project_capabilities(
        context.operations, carto::ai::effect_mask(carto::ai::CapabilityEffect::material));
    carto::ai::NativePlanner planner;
    const auto proposal = planner.propose(
        "assign material 18446744073709551615 to region 7 variant 9", context);
    REQUIRE(proposal);
    REQUIRE(proposal.value().tool_id == "material.assign-region");
    REQUIRE(proposal.value().preview_kind ==
        carto::editor::PreviewKind::set_material_region_assignment);
    REQUIRE(proposal.value().parameters.material_id ==
        std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()});
    REQUIRE(proposal.value().parameters.material_region_id == std::optional<std::uint32_t>{7U});
    REQUIRE(proposal.value().parameters.material_variant_id == std::optional<std::uint64_t>{9U});
    REQUIRE(proposal.value().validate(context));
    REQUIRE(!proposal.value().validate_auto_approval(context));
    const auto json = carto::ai::proposal_json(proposal.value());
    REQUIRE(json.find("\"material_id\":18446744073709551615") != std::string::npos);
    REQUIRE(json.find("\"region_id\":7") != std::string::npos);
    REQUIRE(json.find("\"variant_id\":9") != std::string::npos);

    for (const std::string intent : {
             "assign material 1 to region 7 extra",
             "assign material 1.0 to region 7",
             "assign material 0 to region 7",
             "assign material 1 to region 4294967296",
             "assign material 18446744073709551616 to region 7",
             "assign material 1 to region 7 variant 0"}) {
        REQUIRE(!planner.propose(intent, context));
    }
}

void native_planner_compiles_bounded_lightmap_uv_intents() {
    using namespace carto;
    application::ApplicationSession session;
    REQUIRE(application::HumanApplicationAccess::dispatch(
        session, application::CreateBoxAction{"Root", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(application::HumanApplicationAccess::dispatch(
        session, application::SetSelectionModeAction{editor::SelectionMode::object}));
    REQUIRE(application::HumanApplicationAccess::dispatch(
        session, application::SelectObjectAction{object}));

    auto context = context_for(session);
    context.operations = ai::project_capabilities(
        context.operations, ai::effect_mask(ai::CapabilityEffect::uv));
    ai::NativePlanner planner;
    const auto proposal = planner.propose(
        "generate lightmap uv set 18446744073709551615 resolution 8192 padding 1024 tiles 100",
        context);
    REQUIRE(proposal);
    REQUIRE(proposal.value().tool_id == "uv.generate-lightmap");
    REQUIRE(proposal.value().preview_kind ==
        editor::PreviewKind::generate_unique_lightmap_uv);
    REQUIRE(proposal.value().parameters.lightmap_uv_set_id ==
        std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()});
    REQUIRE(proposal.value().parameters.lightmap_resolution ==
        std::optional<std::uint32_t>{8192U});
    REQUIRE(proposal.value().parameters.lightmap_padding_texels ==
        std::optional<std::uint32_t>{1024U});
    REQUIRE(proposal.value().parameters.lightmap_maximum_udim_tiles ==
        std::optional<std::uint32_t>{100U});
    REQUIRE(proposal.value().validate(context));
    REQUIRE(!proposal.value().validate_auto_approval(context));
    const auto json = ai::proposal_json(proposal.value());
    REQUIRE(json.find("\"uv_set_id\":18446744073709551615") != std::string::npos);
    REQUIRE(json.find("\"resolution\":8192") != std::string::npos);
    REQUIRE(json.find("\"padding_texels\":1024") != std::string::npos);
    REQUIRE(json.find("\"maximum_udim_tiles\":100") != std::string::npos);

    for (const std::string intent : {
             "generate lightmap uv set 0 resolution 128 padding 2 tiles 1",
             "generate lightmap uv set 1 resolution 15 padding 1 tiles 1",
             "generate lightmap uv set 1 resolution 8193 padding 1 tiles 1",
             "generate lightmap uv set 1 resolution 128 padding 17 tiles 1",
             "generate lightmap uv set 1 resolution 128 padding 0 tiles 1",
             "generate lightmap uv set 1 resolution 128 padding 2 tiles 101",
             "generate lightmap uv set 1 resolution 128 padding 2 tiles 0",
             "generate lightmap uv set 18446744073709551616 resolution 128 padding 2 tiles 1",
             "generate lightmap uv set 1 resolution 128.5 padding 2 tiles 1",
             "generate lightmap uv set 1 resolution 128 padding 2 tiles 1 extra"}) {
        REQUIRE(!planner.propose(intent, context));
    }
    context.operations.clear();
    REQUIRE(!planner.propose(
        "generate lightmap uv set 1 resolution 128 padding 2 tiles 1", context));
}

void generic_ai_proposals_cannot_bypass_agent_transform_envelope() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Root", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    const auto before = session.snapshot();

    REQUIRE(!ui.propose_ai("translate object by 0.25 0 0"));
    REQUIRE(!ui.propose_ai("assign material 1 to region 7"));
    REQUIRE(!ui.propose_ai("generate lightmap uv set 1 resolution 128 padding 2 tiles 1"));
    auto full_context = context_for(session);
    carto::ai::NativePlanner planner;
    const auto external_proposal = planner.propose(
        "translate object by 0.25 0 0", full_context);
    REQUIRE(external_proposal);
    REQUIRE(!ui.begin_ai_preview(external_proposal.value()));
    const auto external_material_proposal = planner.propose(
        "assign material 1 to region 7", full_context);
    REQUIRE(external_material_proposal);
    auto uv_context = full_context;
    uv_context.operations = carto::ai::project_capabilities(
        uv_context.operations,
        carto::ai::effect_mask(carto::ai::CapabilityEffect::uv));
    const auto external_uv_proposal = planner.propose(
        "generate lightmap uv set 1 resolution 128 padding 2 tiles 1", uv_context);
    REQUIRE(external_uv_proposal);
    REQUIRE(!ui.begin_ai_preview(external_uv_proposal.value()));
    REQUIRE(!ui.begin_ai_preview(external_material_proposal.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().objects.front().object.local_transform.translation.x == 0.0);
}

void ui_ai_flow_is_preview_only_until_explicit_commit() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{object, face}));

    const auto before = session.snapshot();
    const auto proposal = ui.propose_ai("inset face 0.10");
    REQUIRE(proposal);
    auto preview = ui.begin_ai_preview(proposal.value());
    REQUIRE(preview);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.snapshot().ai_available);

    const auto committed = ui.commit_ai_preview(preview.value(), proposal.value());
    REQUIRE(committed);
    REQUIRE(committed.value().source == carto::application::OperationSource::ai_proposal);
    REQUIRE(committed.value().provenance_id == proposal.value().request_id);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    REQUIRE(session.snapshot().project_revision > before.project_revision);
}

void ui_face_poke_requires_an_explicit_preview_commit() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{object, face}));
    const auto before = session.snapshot();

    REQUIRE(!ui.apply_ai_intent("poke face"));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    const auto proposal = ui.propose_ai("poke face");
    REQUIRE(proposal);
    auto preview = ui.begin_ai_preview(proposal.value());
    REQUIRE(preview);
    REQUIRE(session.snapshot().project_revision == before.project_revision);

    const auto committed = ui.commit_ai_preview(preview.value(), proposal.value());
    REQUIRE(committed);
    REQUIRE(committed.value().action == "Invoke Tool: mesh.poke-face");
    REQUIRE(session.snapshot().project_revision == before.project_revision.next());
}

void native_ai_can_press_auto_apply_through_typed_boundary() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.snapshot().ai_auto_approve);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{object, face}));

    const auto before = session.snapshot();
    const auto applied = ui.apply_ai_intent("inset face 0.10");
    REQUIRE(applied);
    REQUIRE(applied.value().source == carto::application::OperationSource::ai_proposal);
    REQUIRE(!applied.value().provenance_id.empty());
    REQUIRE(session.snapshot().project_revision > before.project_revision);
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
}

void disabled_native_ai_auto_apply_requires_explicit_review() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.set_ai_auto_approve(false));
    REQUIRE(!ui.snapshot().ai_auto_approve);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{object, face}));

    const auto before = session.snapshot();
    const auto before_ui = ui.snapshot();
    const auto blocked = ui.apply_ai_intent("inset face 0.10");
    REQUIRE(!blocked);
    REQUIRE(blocked.error().code == carto::core::ErrorCode::invalid_state);
    const auto after = session.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.objects.size() == before.objects.size());
    REQUIRE(after.viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before.viewport.scene.instances().front().mesh->vertex_ids.size());
    REQUIRE(ui.snapshot().operations.size() == before_ui.operations.size());

    const auto proposal = ui.propose_ai("inset face 0.10");
    REQUIRE(proposal);
    auto preview = ui.begin_ai_preview(proposal.value());
    REQUIRE(preview);
    const auto committed = ui.commit_ai_preview(preview.value(), proposal.value());
    REQUIRE(committed);
    REQUIRE(committed.value().source == carto::application::OperationSource::ai_proposal);
    REQUIRE(session.snapshot().project_revision > before.project_revision);
}

void stale_ai_preview_is_rejected_without_mutation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    const auto face = session.snapshot().viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{object, face}));
    const auto proposal = ui.propose_ai("extrude face 0.20");
    REQUIRE(proposal);
    auto preview = ui.begin_ai_preview(proposal.value());
    REQUIRE(preview);

    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        object,
        carto::core::Transform{{0.5, 0.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}},
        std::nullopt}));
    const auto before_rejected_commit = session.snapshot();
    REQUIRE(!ui.commit_ai_preview(preview.value(), proposal.value()));
    REQUIRE(session.snapshot().project_revision == before_rejected_commit.project_revision);
}

void auto_apply_delegates_invalid_geometry_to_the_kernel_without_mutation() {
    carto::geometry::EditableMesh concave;
    const std::vector<carto::core::Vec3d> positions = {
        {0.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
        {2.0, 2.0, 0.0},
        {1.0, 0.75, 0.0},
        {0.0, 2.0, 0.0},
    };
    std::vector<carto::geometry::VertexId> vertices;
    for (const auto position : positions) {
        const auto added = concave.add_vertex(position);
        REQUIRE(added);
        vertices.push_back(added.value());
    }
    const auto face = concave.add_face(vertices);
    REQUIRE(face);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateMeshObjectAction{
        "Concave", std::move(concave)}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.dispatch(carto::application::SelectFaceAction{object, face.value()}));

    const auto before = session.snapshot();
    const auto applied = ui.apply_ai_intent("inset face 0.10");
    REQUIRE(!applied);
    REQUIRE(applied.error().code == carto::core::ErrorCode::unsupported);
    const auto after = session.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.viewport.scene.instances().front().mesh->vertex_ids.size() ==
            before.viewport.scene.instances().front().mesh->vertex_ids.size());
    REQUIRE(after.viewport.scene.instances().front().mesh->triangle_faces.size() ==
            before.viewport.scene.instances().front().mesh->triangle_faces.size());
}

} // namespace

int main() {
    try {
        ontology_is_derived_from_native_tools();
        ontology_json_preserves_kernel_and_approval_contract();
        capability_projection_filters_by_preview_and_work_envelope();
        ontology_fails_closed_on_tool_kernel_contract_drift();
        native_planner_compiles_a_face_intent_without_mutating_state();
        native_planner_rejects_ambiguous_or_wrong_scope_intents();
        native_planner_compiles_translation_only_for_one_selected_object();
        native_planner_compiles_exact_typed_material_assignment_intents();
        native_planner_compiles_bounded_lightmap_uv_intents();
        generic_ai_proposals_cannot_bypass_agent_transform_envelope();
        ui_ai_flow_is_preview_only_until_explicit_commit();
        ui_face_poke_requires_an_explicit_preview_commit();
        native_ai_can_press_auto_apply_through_typed_boundary();
        disabled_native_ai_auto_apply_requires_explicit_review();
        stale_ai_preview_is_rejected_without_mutation();
        auto_apply_delegates_invalid_geometry_to_the_kernel_without_mutation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
