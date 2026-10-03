#include <carto/ai/ai.hpp>
#include <carto/application/application.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <iostream>
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

    const auto extrude = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.extrude-face"; });
    const auto inset = std::find_if(context.operations.begin(), context.operations.end(),
        [](const auto& operation) { return operation.id == "mesh.inset-face"; });
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
    REQUIRE(extrude != context.operations.end() && extrude->preview_supported);
    REQUIRE(inset != context.operations.end() && inset->preview_supported);
    REQUIRE(poke != context.operations.end() && !poke->preview_supported);
    REQUIRE(delete_face != context.operations.end() && !delete_face->preview_supported);
    REQUIRE(merge_vertices != context.operations.end() && !merge_vertices->preview_supported);
    REQUIRE(dissolve_edge != context.operations.end() && !dissolve_edge->preview_supported);
    REQUIRE(tri_to_quad != context.operations.end() && !tri_to_quad->preview_supported);
    REQUIRE(slide_vertex != context.operations.end() && !slide_vertex->preview_supported);
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
    REQUIRE(poke->precondition.find("future preview") != std::string::npos);
}

void ontology_json_preserves_kernel_and_approval_contract() {
    carto::application::ApplicationSession session;
    const auto context = context_for(session);
    const auto json = carto::ai::context_json(context);
    REQUIRE(json.find("geometry.EditableMesh.inset_face") != std::string::npos);
    REQUIRE(json.find("strictly convex") != std::string::npos);
    REQUIRE(json.find("\"auto_approvable\":true") != std::string::npos);
    REQUIRE(json.find("\"preview_kind\":\"inset_face\"") != std::string::npos);
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
    REQUIRE(operation->precondition.find("do not match") != std::string::npos);
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
    REQUIRE(!planner.propose("move vertex to 1 2 3", context));
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
        carto::core::Transform{{0.5, 0.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}}}));
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
        ontology_fails_closed_on_tool_kernel_contract_drift();
        native_planner_compiles_a_face_intent_without_mutating_state();
        native_planner_rejects_ambiguous_or_wrong_scope_intents();
        ui_ai_flow_is_preview_only_until_explicit_commit();
        native_ai_can_press_auto_apply_through_typed_boundary();
        stale_ai_preview_is_rejected_without_mutation();
        auto_apply_delegates_invalid_geometry_to_the_kernel_without_mutation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
