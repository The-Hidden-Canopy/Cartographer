#include <carto/application/application.hpp>
#include <carto/editor/authoring_context.hpp>
#include <carto/editor/preview_transaction.hpp>
#include <carto/ui/ui.hpp>

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
            carto::core::Transform{{0.0, 3.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}}}));
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
        ui_preview_facade_records_only_the_committed_operation();
        preview_validation_is_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
