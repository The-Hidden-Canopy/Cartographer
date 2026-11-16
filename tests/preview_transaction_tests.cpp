#include <carto/application/application.hpp>
#include <carto/editor/authoring_context.hpp>
#include <carto/editor/preview_transaction.hpp>
#include <carto/ui/ui.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                    \
    do {                                                                                      \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

void authoring_context_rejects_invalid_identity_state() {
    carto::editor::AuthoringContext context;
    context.selection_mode = carto::editor::SelectionMode::object;
    context.objects = {carto::scene::ObjectId{42U}, carto::scene::ObjectId{42U}};
    REQUIRE(!context.validate());

    context.objects = {carto::scene::ObjectId{42U}};
    context.component_object = carto::scene::ObjectId{99U};
    REQUIRE(!context.validate());
}

void preview_updates_are_ephemeral_and_commit_through_admission() {
    carto::application::ApplicationSession session;
    const auto created = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}});
    REQUIRE(created);
    REQUIRE(created.value().operation_id != 0U);
    REQUIRE(created.value().source == carto::application::OperationSource::human);

    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectObjectAction{object}));
    const auto before = session.snapshot();

    auto preview = session.begin_preview(carto::editor::PreviewKind::object_transform);
    REQUIRE(preview);
    REQUIRE(preview.value().active());
    REQUIRE(preview.value().update(carto::editor::PreviewParameters{
        carto::core::Transform{{2.0, 0.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}},
        std::nullopt,
        std::nullopt}));
    REQUIRE(session.snapshot().project_revision == before.project_revision);

    const auto committed = carto::application::HumanApplicationAccess::commit_preview(
        session, preview.value());
    REQUIRE(committed);
    REQUIRE(preview.value().committed());
    REQUIRE(!preview.value().active());
    REQUIRE(session.snapshot().project_revision > before.project_revision);
    REQUIRE(session.snapshot().objects.front().object.local_transform.translation.x == 2.0);
}

void stale_preview_is_refused_without_a_second_mutation() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectObjectAction{object}));
    auto preview = session.begin_preview(carto::editor::PreviewKind::object_transform);
    REQUIRE(preview);
    REQUIRE(preview.value().update(carto::editor::PreviewParameters{
        carto::core::Transform{{1.0, 2.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}},
        std::nullopt,
        std::nullopt}));

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetObjectTransformAction{
            object,
            carto::core::Transform{{0.0, 3.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}},
            std::nullopt}));
    const auto before_rejected_commit = session.snapshot();
    const auto rejected = carto::application::HumanApplicationAccess::commit_preview(
        session, preview.value());
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(session.snapshot().project_revision == before_rejected_commit.project_revision);
    REQUIRE(preview.value().active());
}

void component_preview_emits_affected_ids_and_parameters() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto before_selection = session.snapshot();
    const auto object = before_selection.objects.front().object.id;
    const auto face = before_selection.viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session,
        carto::application::SetSelectionModeAction{carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, face}));

    auto preview = session.begin_preview(carto::editor::PreviewKind::extrude_face);
    REQUIRE(preview);
    REQUIRE(preview.value().update(carto::editor::PreviewParameters{
        std::nullopt, 0.25, std::nullopt}));
    const auto committed = carto::application::HumanApplicationAccess::commit_preview(
        session, preview.value());
    REQUIRE(committed);
    REQUIRE(committed.value().affected.objects.size() == 1U);
    REQUIRE(committed.value().affected.objects.front() == object);
    REQUIRE(committed.value().affected.faces.size() == 1U);
    REQUIRE(committed.value().affected.faces.front() == face);
    REQUIRE(committed.value().parameters.has_value());
    REQUIRE(committed.value().parameters->distance.has_value());
    REQUIRE(*committed.value().parameters->distance == 0.25);
}

void face_poke_preview_is_parameterless_and_revision_bound() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto initial = session.snapshot();
    const auto object = initial.objects.front().object.id;
    const auto face = initial.viewport.scene.instances().front().mesh->triangle_faces.front();
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SetSelectionModeAction{
            carto::editor::SelectionMode::face}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SelectFaceAction{object, face}));
    const auto before = session.snapshot();
    const auto* before_instance = before.viewport.scene.find(object);
    REQUIRE(before_instance != nullptr && before_instance->mesh != nullptr);

    auto preview = session.begin_preview(carto::editor::PreviewKind::poke_face);
    REQUIRE(preview);
    REQUIRE(!preview.value().update(carto::editor::PreviewParameters{
        std::nullopt, 0.1, std::nullopt}));
    REQUIRE(preview.value().active());
    REQUIRE(preview.value().update(carto::editor::PreviewParameters{}));
    REQUIRE(session.snapshot().project_revision == before.project_revision);

    const auto committed = carto::application::HumanApplicationAccess::commit_preview(
        session, preview.value());
    REQUIRE(committed);
    const auto after = session.snapshot();
    const auto* after_instance = after.viewport.scene.find(object);
    REQUIRE(after_instance != nullptr && after_instance->mesh != nullptr);
    REQUIRE(after_instance->mesh->vertex_ids.size() ==
        before_instance->mesh->vertex_ids.size() + 1U);
    REQUIRE(after_instance->mesh->triangle_faces.size() >
        before_instance->mesh->triangle_faces.size());
    REQUIRE(after.project_revision == before.project_revision.next());
}

void material_assignment_preview_requires_exact_typed_ids() {
    carto::editor::PreviewParameters parameters;
    parameters.material_region_id = 7U;
    parameters.material_id = 11U;
    REQUIRE(parameters.validate(
        carto::editor::PreviewKind::set_material_region_assignment));
    parameters.material_variant_id = 3U;
    REQUIRE(parameters.validate(
        carto::editor::PreviewKind::set_material_region_assignment));

    parameters.material_region_id = 0U;
    REQUIRE(!parameters.validate(
        carto::editor::PreviewKind::set_material_region_assignment));
    parameters.material_region_id = 7U;
    parameters.material_id = 0U;
    REQUIRE(!parameters.validate(
        carto::editor::PreviewKind::set_material_region_assignment));
    parameters.material_id = 11U;
    parameters.distance = 0.1;
    REQUIRE(!parameters.validate(
        carto::editor::PreviewKind::set_material_region_assignment));
    parameters.distance.reset();
    REQUIRE(!parameters.validate(carto::editor::PreviewKind::object_translation_delta));
}

void unique_lightmap_uv_preview_is_typed_bounded_and_object_scoped() {
    using namespace carto;
    editor::PreviewParameters parameters;
    parameters.lightmap_uv_set_id = 17U;
    parameters.lightmap_resolution = 128U;
    parameters.lightmap_padding_texels = 16U;
    parameters.lightmap_maximum_udim_tiles = 100U;
    REQUIRE(parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));

    parameters.lightmap_uv_set_id = 0U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_uv_set_id = 17U;
    parameters.lightmap_resolution = 15U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_resolution = 8193U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_resolution = 128U;
    parameters.lightmap_padding_texels = 17U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_padding_texels = 0U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_padding_texels = 16U;
    parameters.lightmap_maximum_udim_tiles = 0U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_maximum_udim_tiles = 101U;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    parameters.lightmap_maximum_udim_tiles = 100U;
    parameters.distance = 0.5;
    REQUIRE(!parameters.validate(editor::PreviewKind::generate_unique_lightmap_uv));
    REQUIRE(!parameters.validate(editor::PreviewKind::object_transform));

    editor::AuthoringContext context;
    context.selection_mode = editor::SelectionMode::object;
    context.objects = {scene::ObjectId{7U}};
    REQUIRE(editor::AuthoringPreview::begin(
        core::Revision{4U}, context, editor::PreviewKind::generate_unique_lightmap_uv));
    context.objects.push_back(scene::ObjectId{8U});
    REQUIRE(!editor::AuthoringPreview::begin(
        core::Revision{4U}, context, editor::PreviewKind::generate_unique_lightmap_uv));
}

void ui_preview_facade_records_only_the_committed_operation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto object = ui.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));

    auto preview = ui.begin_preview(carto::editor::PreviewKind::object_transform);
    REQUIRE(preview);
    REQUIRE(preview.value().update(carto::editor::PreviewParameters{
        carto::core::Transform{{4.0, 0.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}},
        std::nullopt,
        std::nullopt}));
    REQUIRE(ui.snapshot().operations.size() == 1U);
    REQUIRE(ui.commit_preview(preview.value()));
    REQUIRE(ui.snapshot().operations.size() == 2U);
    REQUIRE(ui.snapshot().operations.back().action == "Set Object Transform");
    REQUIRE(ui.snapshot().objects.front().object.local_transform.translation.x == 4.0);
}

void vertex_slide_preview_cancellation_staleness_and_support_are_atomic() {
    using namespace carto;
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::CreateBoxAction{"Slide", {1.0, 1.0, 1.0}}));
    REQUIRE(!session.begin_preview(editor::PreviewKind::slide_vertex));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(application::SelectVertexAction{object, geometry::VertexId{1U}}));
    const auto before = session.snapshot();
    editor::PreviewParameters parameters;
    parameters.factor = 0.25;
    auto canceled = session.begin_preview(editor::PreviewKind::slide_vertex);
    REQUIRE(canceled);
    REQUIRE(canceled.value().update(parameters));
    canceled.value().cancel();
    REQUIRE(!ui.commit_preview(canceled.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    auto invalid_edge = session.begin_preview(editor::PreviewKind::slide_vertex);
    REQUIRE(invalid_edge);
    parameters.support_edge = geometry::EdgeId{999999U};
    REQUIRE(invalid_edge.value().update(parameters));
    REQUIRE(!ui.commit_preview(invalid_edge.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    auto stale = session.begin_preview(editor::PreviewKind::slide_vertex);
    REQUIRE(stale);
    parameters.support_edge.reset();
    REQUIRE(stale.value().update(parameters));
    auto valid = session.begin_preview(editor::PreviewKind::slide_vertex);
    REQUIRE(valid);
    REQUIRE(valid.value().update(parameters));
    REQUIRE(ui.commit_preview(valid.value()));
    REQUIRE(!ui.commit_preview(valid.value()));
    REQUIRE(!ui.commit_preview(stale.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision.next());

    REQUIRE(ui.dispatch(application::SelectVertexAction{object, geometry::VertexId{1U}}));
    auto old_generation = session.begin_preview(editor::PreviewKind::slide_vertex);
    REQUIRE(old_generation);
    REQUIRE(old_generation.value().update(parameters));
    const auto temporary = std::filesystem::temp_directory_path() /
        ("cartographer-slide-generation-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temporary);
    const auto project_path = temporary / "generation.carto";
    REQUIRE(ui.dispatch(application::SaveProjectAction{project_path}));
    const auto generation_before_reopen = session.snapshot().project_generation;
    REQUIRE(ui.dispatch(application::OpenProjectAction{project_path, true}));
    REQUIRE(session.snapshot().project_generation != generation_before_reopen);
    const auto replacement = session.snapshot();
    REQUIRE(replacement.project_revision == old_generation.value().base_revision());
    REQUIRE(!ui.commit_preview(old_generation.value()));
    REQUIRE(session.snapshot().project_revision == replacement.project_revision);
}

void edge_split_preview_cancellation_staleness_and_support_are_atomic() {
    using namespace carto;
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::CreateBoxAction{"Slide", {1.0, 1.0, 1.0}}));
    REQUIRE(!session.begin_preview(editor::PreviewKind::split_edge));
    const auto object = session.snapshot().objects.front().object.id;
    REQUIRE(ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::edge}));
    REQUIRE(ui.dispatch(application::SelectEdgeAction{object, geometry::EdgeId{1U}}));
    const auto before = session.snapshot();
    editor::PreviewParameters parameters;
    parameters.factor = 0.25;
    auto canceled = session.begin_preview(editor::PreviewKind::split_edge);
    REQUIRE(canceled);
    REQUIRE(canceled.value().update(parameters));
    canceled.value().cancel();
    REQUIRE(!ui.commit_preview(canceled.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    auto invalid_edge = session.begin_preview(editor::PreviewKind::split_edge);
    REQUIRE(invalid_edge);
    parameters.support_edge = geometry::EdgeId{999999U};
    REQUIRE(!invalid_edge.value().update(parameters));
    REQUIRE(!ui.commit_preview(invalid_edge.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    auto stale = session.begin_preview(editor::PreviewKind::split_edge);
    REQUIRE(stale);
    parameters.support_edge.reset();
    REQUIRE(stale.value().update(parameters));
    auto valid = session.begin_preview(editor::PreviewKind::split_edge);
    REQUIRE(valid);
    REQUIRE(valid.value().update(parameters));
    REQUIRE(ui.commit_preview(valid.value()));
    REQUIRE(!ui.commit_preview(valid.value()));
    REQUIRE(!ui.commit_preview(stale.value()));
    REQUIRE(session.snapshot().project_revision == before.project_revision.next());

    std::optional<geometry::EdgeId> remaining_edge;
    for (const auto& triangle : session.snapshot().viewport.scene.instances().front().mesh->triangle_edges) {
        for (const auto& edge : triangle) if (edge) remaining_edge = edge;
    }
    REQUIRE(remaining_edge);
    REQUIRE(ui.dispatch(application::SelectEdgeAction{object, *remaining_edge}));
    auto old_generation = session.begin_preview(editor::PreviewKind::split_edge);
    REQUIRE(old_generation);
    REQUIRE(old_generation.value().update(parameters));
    const auto temporary = std::filesystem::temp_directory_path() /
        ("cartographer-split-generation-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temporary);
    const auto project_path = temporary / "generation.carto";
    REQUIRE(ui.dispatch(application::SaveProjectAction{project_path}));
    const auto generation_before_reopen = session.snapshot().project_generation;
    REQUIRE(ui.dispatch(application::OpenProjectAction{project_path, true}));
    REQUIRE(session.snapshot().project_generation != generation_before_reopen);
    const auto replacement = session.snapshot();
    REQUIRE(replacement.project_revision == old_generation.value().base_revision());
    REQUIRE(!ui.commit_preview(old_generation.value()));
    REQUIRE(session.snapshot().project_revision == replacement.project_revision);
}

void preview_validation_is_fail_closed() {
    carto::editor::AuthoringContext context;
    context.selection_mode = carto::editor::SelectionMode::object;
    context.objects = {carto::scene::ObjectId{7U}};
    REQUIRE(context.validate());
    auto preview = carto::editor::AuthoringPreview::begin(
        carto::core::Revision{4U}, context, carto::editor::PreviewKind::object_transform);
    REQUIRE(preview);
    REQUIRE(!preview.value().update(carto::editor::PreviewParameters{}));
    REQUIRE(preview.value().active());
    preview.value().cancel();
    REQUIRE(!preview.value().validate_commit(carto::core::Revision{4U}));
}

} // namespace

int main() {
    try {
        authoring_context_rejects_invalid_identity_state();
        preview_updates_are_ephemeral_and_commit_through_admission();
        stale_preview_is_refused_without_a_second_mutation();
        component_preview_emits_affected_ids_and_parameters();
        face_poke_preview_is_parameterless_and_revision_bound();
        material_assignment_preview_requires_exact_typed_ids();
        unique_lightmap_uv_preview_is_typed_bounded_and_object_scoped();
        ui_preview_facade_records_only_the_committed_operation();
        vertex_slide_preview_cancellation_staleness_and_support_are_atomic();
        edge_split_preview_cancellation_staleness_and_support_are_atomic();
        preview_validation_is_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
