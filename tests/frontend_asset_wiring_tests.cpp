#include <carto/ui/ui.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/assets/source_registry.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw std::runtime_error(std::string("requirement failed: ") +   \
            #condition);                                                                 \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-frontend-asset-tests-" + std::to_string(stamp));
        REQUIRE(std::filesystem::create_directories(path_));
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

std::span<const std::uint8_t> byte_span(std::string_view bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

std::filesystem::path journal_path(const std::filesystem::path& path) {
    return std::filesystem::path(path.string() + ".journal");
}

struct PublicationState {
    carto::application::ApplicationSnapshot snapshot;
    std::string document_bytes;
    std::string journal_bytes;
    std::size_t operation_count = 0U;
};

PublicationState capture_publication(
    carto::ui::UiController& ui,
    const carto::application::ApplicationSession& session,
    const std::filesystem::path& path) {
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    return {session.snapshot(), read_bytes(path), read_bytes(journal_path(path)),
        ui.snapshot().operations.size()};
}

void require_unchanged_publication(
    carto::ui::UiController& ui,
    const carto::application::ApplicationSession& session,
    const std::filesystem::path& path,
    const PublicationState& before) {
    const auto after = session.snapshot();
    REQUIRE(after.project_generation == before.snapshot.project_generation);
    REQUIRE(after.project_revision == before.snapshot.project_revision);
    REQUIRE(after.project_path == before.snapshot.project_path);
    REQUIRE(after.undo_count == before.snapshot.undo_count);
    REQUIRE(after.redo_count == before.snapshot.redo_count);
    REQUIRE(after.dirty == before.snapshot.dirty);
    REQUIRE(read_bytes(path) == before.document_bytes);
    REQUIRE(read_bytes(journal_path(path)) == before.journal_bytes);
    // A public save exposes the complete current document for exact comparison
    // without granting these frontend tests private application access.
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    REQUIRE(read_bytes(path) == before.document_bytes);
}

template <typename Operation>
void expect_refused_without_mutation(
    carto::ui::UiController& ui,
    const carto::application::ApplicationSession& session,
    const std::filesystem::path& path,
    Operation operation,
    std::optional<carto::core::ErrorCode> expected_error = std::nullopt) {
    const auto before = capture_publication(ui, session, path);
    const auto result = operation();
    REQUIRE(!result);
    if (expected_error.has_value()) REQUIRE(result.error().code == *expected_error);
    REQUIRE(ui.snapshot().operations.size() == before.operation_count);
    require_unchanged_publication(ui, session, path, before);
}

void require_operation_rail_entry(
    const carto::ui::UiController& ui,
    const carto::application::DispatchReceipt& receipt) {
    const auto snapshot = ui.snapshot();
    REQUIRE(std::any_of(snapshot.operations.begin(), snapshot.operations.end(),
        [&](const auto& operation) {
            return operation.category == carto::ui::OperationCategory::committed_command &&
                operation.action == receipt.action && operation.document_changed &&
                operation.revision_before == receipt.revision_before &&
                operation.revision_after == receipt.revision_after;
        }));
    REQUIRE(receipt.source == carto::application::OperationSource::human);
}

void require_one_journal_event(
    const std::filesystem::path& path,
    std::string_view action) {
    const auto entries = carto::journal::Journal(journal_path(path)).read_all();
    REQUIRE(entries);
    REQUIRE(std::count_if(entries.value().begin(), entries.value().end(),
        [&](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find(action) != std::string::npos;
        }) == 1);
}

void frontend_generates_lightmap_uv_for_exact_current_selection() {
    using namespace carto;
    TempDirectory directory;
    const auto path = directory.path() / "lightmap.carto";
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::NewProjectAction{"Frontend lightmap"}));
    REQUIRE(ui.dispatch(application::CreateBoxAction{"Target", {1.0, 1.0, 1.0}}));
    REQUIRE(ui.dispatch(application::CreateBoxAction{"Other", {1.0, 1.0, 1.0}}));
    const auto source = session.snapshot();
    REQUIRE(source.objects.size() == 2U);
    const auto target = source.objects.front().object.id;
    const auto other = source.objects.back().object.id;
    const auto mesh_asset = source.objects.front().object.mesh_asset;
    REQUIRE(mesh_asset.has_value());
    REQUIRE(ui.dispatch(application::SelectObjectAction{target}));
    application::GenerateUniqueLightmapUvAction action{
        source.project_revision, target, 9U, 128U, 2U, 1U};

    auto excessive_padding = action;
    excessive_padding.padding_texels = action.resolution / 8U + 1U;
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(excessive_padding, source.project_generation);
    }, core::ErrorCode::invalid_argument);
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(action, source.project_generation + 1U);
    }, core::ErrorCode::stale_data);
    auto stale_action = action;
    stale_action.expected_project_revision = action.expected_project_revision.next();
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(stale_action, source.project_generation);
    }, core::ErrorCode::stale_data);
    REQUIRE(ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::face}));
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(action, source.project_generation);
    }, core::ErrorCode::invalid_state);
    REQUIRE(ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::object}));
    REQUIRE(ui.dispatch(application::SelectObjectAction{other}));
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(action, source.project_generation);
    }, core::ErrorCode::invalid_state);
    REQUIRE(ui.dispatch(application::SelectObjectAction{target}));
    REQUIRE(ui.dispatch(application::SelectObjectAction{other, editor::SelectionOperation::add}));
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(action, source.project_generation);
    }, core::ErrorCode::invalid_state);
    REQUIRE(ui.dispatch(application::SelectObjectAction{target}));
    const auto before = capture_publication(ui, session, path);
    const auto generated = ui.generate_unique_lightmap_uv(action, source.project_generation);
    REQUIRE(generated && generated.value().document_changed);
    REQUIRE(session.snapshot().undo_count == before.snapshot.undo_count + 1U);
    require_operation_rail_entry(ui, generated.value());
    require_one_journal_event(path, "Generate Unique Lightmap UVs ");

    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    const auto generated_document = project::ProjectDocument::load(path);
    REQUIRE(generated_document);
    const auto& generated_mesh = generated_document.value().meshes().at(*mesh_asset);
    const auto* uv_set = generated_mesh.uv_sets().find(9U);
    REQUIRE(uv_set != nullptr && uv_set->lightmap_candidate);
    REQUIRE(generated_mesh.attributes().find("uv.lightmap.9") != nullptr);
    REQUIRE(generated_mesh.attributes().find("uv.agent.lightmap.9") == nullptr);
    REQUIRE(ui.dispatch(application::UndoAction{}));
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    const auto undone = project::ProjectDocument::load(path);
    REQUIRE(undone && undone.value().meshes().at(*mesh_asset).uv_sets().find(9U) == nullptr);
    REQUIRE(ui.dispatch(application::RedoAction{}));
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    REQUIRE(ui.dispatch(application::OpenProjectAction{path}));
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto* restored_set = reopened.value().meshes().at(*mesh_asset).uv_sets().find(9U);
    REQUIRE(restored_set != nullptr && restored_set->lightmap_candidate);
    REQUIRE(reopened.value().meshes().at(*mesh_asset).attributes().find(
        "uv.lightmap.9") != nullptr);
}

void frontend_refuses_lightmap_uv_when_another_locked_instance_shares_the_mesh() {
    using namespace carto;
    TempDirectory directory;
    const auto path = directory.path() / "locked-shared-lightmap.carto";
    auto document = project::ProjectDocument::create("Locked shared lightmap fixture");
    REQUIRE(document);
    journal::Journal fixture_journal(directory.path() / "fixture-authoring.journal");
    const std::string initial_snapshot = document.value().serialize();
    REQUIRE(fixture_journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        byte_span(initial_snapshot), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), fixture_journal, "frontend-asset-tests");
    REQUIRE(transaction);
    auto mesh = geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(mesh);
    const auto mesh_asset = transaction.value().add_mesh(mesh.value());
    REQUIRE(mesh_asset);
    scene::SceneObject target;
    target.id = scene::ObjectId{1U};
    target.name = "Unlocked selected instance";
    target.mesh_asset = mesh_asset.value();
    REQUIRE(transaction.value().insert_object(target));
    auto locked_instance = target;
    locked_instance.id = scene::ObjectId{2U};
    locked_instance.name = "Locked shared instance";
    locked_instance.locked = true;
    REQUIRE(transaction.value().insert_object(locked_instance));
    REQUIRE(transaction.value().commit("frontend.locked-shared-lightmap-fixture"));
    REQUIRE(document.value().save_atomic(path));

    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::OpenProjectAction{path}));
    REQUIRE(ui.dispatch(application::SelectObjectAction{target.id}));
    const auto source = session.snapshot();
    REQUIRE(source.objects.size() == 2U && source.selection.objects.size() == 1U);
    REQUIRE(source.objects.front().object.mesh_asset == source.objects.back().object.mesh_asset);
    REQUIRE(!source.objects.front().object.locked && source.objects.back().object.locked);
    const application::GenerateUniqueLightmapUvAction action{
        source.project_revision, target.id, 9U, 128U, 2U, 1U};
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.generate_unique_lightmap_uv(action, source.project_generation);
    }, core::ErrorCode::invalid_state);
}

void frontend_relinks_only_exact_moved_source_bytes_with_retained_lineage() {
    using namespace carto;
    TempDirectory directory;
    const auto path = directory.path() / "source-relink.carto";
    const std::string source_bytes = "o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const auto moved_path = directory.path() / "moved.obj";
    const auto changed_path = directory.path() / "changed.obj";
    write_bytes(moved_path, source_bytes);
    write_bytes(changed_path, source_bytes + "# changed source bytes\n");
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::NewProjectAction{"Frontend moved source"}));
    const auto unsaved = session.snapshot();
    const auto requires_saved_project = ui.prepare_moved_source_relink(
        "asset.triangle", moved_path, unsaved.project_generation, unsaved.project_revision);
    REQUIRE(!requires_saved_project &&
        requires_saved_project.error().code == core::ErrorCode::invalid_state);
    REQUIRE(session.snapshot().project_revision == unsaved.project_revision &&
        session.snapshot().undo_count == unsaved.undo_count);
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));

    geometry::EditableMesh triangle;
    const auto a = triangle.add_vertex({0.0, 0.0, 0.0});
    const auto b = triangle.add_vertex({1.0, 0.0, 0.0});
    const auto c = triangle.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(triangle.add_face({a.value(), b.value(), c.value()}));
    const auto artifact_digest = application::canonical_mesh_geometry_digest(triangle);
    REQUIRE(ui.dispatch(application::CreateMeshObjectAction{"Triangle", triangle}));
    assets::SourceImportRecord original;
    original.identity = "asset.triangle";
    original.relative_source_path = "Assets/original.obj";
    original.source_generation = core::Revision{1U};
    original.kind = assets::SourceAssetKind::mesh;
    original.source_digest = assets::sha256(byte_span(source_bytes));
    original.importer_identity = "carto.import.obj";
    original.importer_version = 1U;
    original.artifact_identity = "mesh.triangle";
    original.artifact_digest = artifact_digest;
    original.domain_digests = {{assets::SourceAssetDomain::geometry, artifact_digest}};
    const auto original_path = directory.path() / original.relative_source_path;
    REQUIRE(std::filesystem::create_directories(original_path.parent_path()));
    write_bytes(original_path, source_bytes);
    const auto import_plan = ui.preview_source_reimport({original});
    REQUIRE(import_plan);
    REQUIRE(ui.apply_source_reimport_plan(import_plan.value()));
    REQUIRE(std::filesystem::remove(original_path));
    const auto source = session.snapshot();
    REQUIRE(source.source_assets.size() == 1U);

    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.prepare_moved_source_relink(original.identity, moved_path,
            source.project_generation + 1U, source.project_revision);
    }, core::ErrorCode::stale_data);
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.prepare_moved_source_relink(original.identity, moved_path,
            source.project_generation, source.project_revision.next());
    }, core::ErrorCode::stale_data);
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.prepare_moved_source_relink(original.identity, changed_path,
            source.project_generation, source.project_revision);
    }, core::ErrorCode::validation_failed);
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.prepare_moved_source_relink("asset.absent", moved_path,
            source.project_generation, source.project_revision);
    }, core::ErrorCode::not_found);

    const auto before_prepare = capture_publication(ui, session, path);
    const auto prepared = ui.prepare_moved_source_relink(original.identity, moved_path,
        source.project_generation, source.project_revision);
    REQUIRE(prepared && prepared.value().identity == original.identity &&
        prepared.value().staged_digest == original.source_digest);
    require_unchanged_publication(ui, session, path, before_prepare);
    assets::BlobStore store(directory.path() / ".carto-assets" / "source-blobs");
    REQUIRE(store.verify(original.source_digest));
    REQUIRE(read_bytes(store.path_for(original.source_digest)) == source_bytes);
    const auto staged_relative_path = store.path_for(original.source_digest)
        .lexically_relative(directory.path()).generic_string();

    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.relink_moved_source(prepared.value(), source.project_generation + 1U,
            source.project_revision);
    }, core::ErrorCode::stale_data);
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.relink_moved_source(prepared.value(), source.project_generation,
            source.project_revision.next());
    }, core::ErrorCode::stale_data);
    const auto wrong_blob = store.put(
        byte_span(read_bytes(changed_path)), "application/octet-stream");
    REQUIRE(wrong_blob);
    expect_refused_without_mutation(ui, session, path, [&] {
        return ui.relink_moved_source(application::RelinkMovedSourceAction{
            original.identity, wrong_blob.value().digest}, source.project_generation,
            source.project_revision);
    }, core::ErrorCode::validation_failed);

    const auto before_relink = capture_publication(ui, session, path);
    const auto relinked = ui.relink_moved_source(prepared.value(), source.project_generation,
        source.project_revision);
    REQUIRE(relinked && relinked.value().document_changed);
    require_operation_rail_entry(ui, relinked.value());
    require_one_journal_event(path, "Relink Moved Source ");
    const auto after = session.snapshot();
    REQUIRE(after.undo_count == before_relink.snapshot.undo_count + 1U);
    REQUIRE(after.source_assets.size() == 1U);
    const auto& relinked_source = after.source_assets.front();
    REQUIRE(relinked_source.relative_source_path == staged_relative_path);
    REQUIRE(relinked_source.source_digest == original.source_digest);
    REQUIRE(relinked_source.artifact_digest == original.artifact_digest);
    REQUIRE(relinked_source.source_generation > original.source_generation);

    REQUIRE(ui.dispatch(application::UndoAction{}));
    const auto undone = session.snapshot().source_assets.front();
    REQUIRE(undone.relative_source_path == original.relative_source_path);
    REQUIRE(undone.source_generation > relinked_source.source_generation);
    REQUIRE(undone.source_digest == original.source_digest);
    REQUIRE(ui.dispatch(application::RedoAction{}));
    const auto redone = session.snapshot().source_assets.front();
    REQUIRE(redone.relative_source_path == staged_relative_path);
    REQUIRE(redone.source_generation > undone.source_generation);
    REQUIRE(redone.source_digest == original.source_digest);
    REQUIRE(read_bytes(store.path_for(original.source_digest)) == source_bytes);
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    REQUIRE(ui.dispatch(application::OpenProjectAction{path}));
    REQUIRE(session.snapshot().source_assets.front().relative_source_path == staged_relative_path);
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto& registry = reopened.value().source_asset_registry();
    const auto& persisted = registry.records().at(original.identity);
    REQUIRE(persisted.source_generation == redone.source_generation);
    REQUIRE(registry.lineage().at(original.identity) == persisted.source_generation);
    REQUIRE(persisted.relative_source_path == staged_relative_path);
    REQUIRE(persisted.source_digest == original.source_digest);
    REQUIRE(persisted.artifact_digest == original.artifact_digest);
}

void frontend_restores_native_import_source_without_registry_or_journal_mutation() {
    using namespace carto;
    TempDirectory directory;
    const auto path = directory.path() / "native-source-restoration.carto";
    const auto original_path = directory.path() / "original.obj";
    const auto moved_path = directory.path() / "moved-source.obj";
    const std::string source_bytes = "o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    write_bytes(original_path, source_bytes);
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::NewProjectAction{"Native source restoration"}));
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    const auto imported = ui.import_mesh(original_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported && imported.value().source_linked);
    const auto source = session.snapshot();
    REQUIRE(source.source_assets.size() == 1U);
    const auto& registered = source.source_assets.front();
    assets::BlobStore store(directory.path() / ".carto-assets" / "source-blobs");
    const auto managed_blob = store.path_for(registered.source_digest);
    REQUIRE(registered.relative_source_path ==
        managed_blob.lexically_relative(directory.path()).generic_string());
    REQUIRE(store.verify(registered.source_digest));
    const auto before = capture_publication(ui, session, path);
    std::filesystem::rename(managed_blob, moved_path);
    REQUIRE(!std::filesystem::exists(managed_blob));
    REQUIRE(read_bytes(moved_path) == source_bytes);

    const auto prepared = ui.prepare_moved_source_relink(registered.identity, moved_path,
        source.project_generation, source.project_revision);
    REQUIRE(prepared && prepared.value().staged_digest == registered.source_digest);
    REQUIRE(store.verify(registered.source_digest));
    REQUIRE(read_bytes(managed_blob) == source_bytes);
    require_unchanged_publication(ui, session, path, before);
    const auto restored = ui.relink_moved_source(prepared.value(), source.project_generation,
        source.project_revision);
    REQUIRE(restored && !restored.value().document_changed);
    REQUIRE(restored.value().source == application::OperationSource::human);
    REQUIRE(restored.value().revision_before == source.project_revision);
    REQUIRE(restored.value().revision_after == source.project_revision);
    require_unchanged_publication(ui, session, path, before);
    REQUIRE(store.verify(registered.source_digest));
    REQUIRE(read_bytes(managed_blob) == source_bytes);
    REQUIRE(read_bytes(moved_path) == source_bytes);
}

void frontend_refuses_internal_source_symlink_substitution_after_relink_review() {
    using namespace carto;
    TempDirectory directory;
    const auto path = directory.path() / "source-link-substitution.carto";
    const auto original_path = directory.path() / "original.obj";
    const auto internal_alias = directory.path() / "unreviewed-source.obj";
    const std::string source_bytes = "o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    write_bytes(original_path, source_bytes);
    write_bytes(internal_alias, source_bytes);
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::NewProjectAction{"Source link substitution"}));
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    const auto imported = ui.import_mesh(original_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported && imported.value().source_linked);
    const auto source = session.snapshot();
    REQUIRE(source.source_assets.size() == 1U);
    const auto& registered = source.source_assets.front();
    const auto prepared = ui.prepare_moved_source_relink(registered.identity, original_path,
        source.project_generation, source.project_revision);
    REQUIRE(prepared);
    const auto before = capture_publication(ui, session, path);
    assets::BlobStore store(directory.path() / ".carto-assets" / "source-blobs");
    const auto managed_blob = store.path_for(registered.source_digest);
    REQUIRE(std::filesystem::remove(managed_blob));
    std::error_code error;
    std::filesystem::create_symlink(internal_alias, managed_blob, error);
    if (error) {
        std::cout << "SKIP: internal source symlink substitution case unavailable: "
            << error.message() << '\n';
        return;
    }
    REQUIRE(std::filesystem::is_symlink(std::filesystem::symlink_status(managed_blob)));
    REQUIRE(std::filesystem::equivalent(managed_blob, internal_alias));
    // Equal verified bytes inside the project must not authorize a different locator.
    REQUIRE(store.verify(registered.source_digest));
    const auto relinked = ui.relink_moved_source(prepared.value(), source.project_generation,
        source.project_revision);
    REQUIRE(!relinked && relinked.error().code == core::ErrorCode::validation_failed);
    require_unchanged_publication(ui, session, path, before);
    REQUIRE(read_bytes(internal_alias) == source_bytes);
}

void frontend_replays_import_and_metadata_history_with_retained_source_lineage() {
    using namespace carto;
    TempDirectory directory;
    const auto path = directory.path() / "ordered-source-history.carto";
    const auto source_path = directory.path() / "triangle.obj";
    write_bytes(source_path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    application::ApplicationSession session;
    ui::UiController ui(session);
    REQUIRE(ui.dispatch(application::SaveProjectAction{path}));
    auto entries = journal::Journal(journal_path(path)).read_all();
    REQUIRE(entries);
    std::size_t event_count = entries.value().size();
    const auto check_event = [&] {
        const auto current = journal::Journal(journal_path(path)).read_all();
        REQUIRE(current && current.value().size() == ++event_count);
    };
    REQUIRE(ui.import_mesh(source_path, ui::MeshImportFormat::obj));
    check_event();
    const auto imported = session.snapshot();
    REQUIRE(imported.source_assets.size() == 1U && imported.objects.size() == 1U);
    const auto identity = imported.source_assets.front().identity;
    const auto first_object = imported.objects.front().object.id;
    const auto first_mesh = imported.objects.front().object.mesh_asset;
    const auto prepare_tags = [&](std::vector<std::string> tags) {
        const auto snapshot = session.snapshot();
        return ui.preview_source_asset_metadata(identity,
            snapshot.source_assets.front().source_generation,
            snapshot.project_generation, snapshot.project_revision, std::move(tags), {});
    };
    const auto first_review = prepare_tags({"qa"});
    REQUIRE(first_review && ui.apply_source_reimport_plan(first_review.value()));
    check_event();
    const auto second_review = prepare_tags({"qa", "synthetic"});
    REQUIRE(second_review && ui.apply_source_reimport_plan(second_review.value()));
    check_event();
    auto generation = session.snapshot().source_assets.front().source_generation;
    for (const std::vector<std::string>& tags :
         {std::vector<std::string>{"qa"}, std::vector<std::string>{}}) {
        const auto before = session.snapshot();
        REQUIRE(ui.dispatch(application::UndoAction{}));
        const auto after = session.snapshot();
        REQUIRE(after.project_revision > before.project_revision &&
            after.source_assets.front().source_generation > generation &&
            after.source_assets.front().authored_tags == tags);
        generation = after.source_assets.front().source_generation;
        check_event();
    }
    REQUIRE(ui.dispatch(application::UndoAction{}));
    REQUIRE(session.snapshot().source_assets.empty() && session.snapshot().objects.empty());
    check_event();
    REQUIRE(ui.dispatch(application::RedoAction{}));
    const auto restored = session.snapshot();
    REQUIRE(restored.source_assets.size() == 1U && restored.objects.size() == 1U &&
        restored.source_assets.front().source_generation > generation &&
        restored.objects.front().object.id == first_object &&
        restored.objects.front().object.mesh_asset == first_mesh);
    check_event();
    for (const std::vector<std::string>& tags :
         {std::vector<std::string>{"qa"}, std::vector<std::string>{"qa", "synthetic"}}) {
        const auto before = session.snapshot();
        REQUIRE(ui.dispatch(application::RedoAction{}));
        const auto after = session.snapshot();
        REQUIRE(after.project_revision > before.project_revision &&
            after.source_assets.front().source_generation >
                before.source_assets.front().source_generation &&
            after.source_assets.front().authored_tags == tags);
        check_event();
    }
    const auto before_failed_append = capture_publication(ui, session, path);
    const auto failed_review = prepare_tags({"unpublished"});
    REQUIRE(failed_review);
    const auto journal_backup = directory.path() / "journal-backup";
    std::filesystem::rename(journal_path(path), journal_backup);
    REQUIRE(std::filesystem::create_directory(journal_path(path)));
    const auto failed_append = ui.apply_source_reimport_plan(failed_review.value());
    REQUIRE(std::filesystem::remove(journal_path(path)));
    std::filesystem::rename(journal_backup, journal_path(path));
    REQUIRE(!failed_append);
    require_unchanged_publication(ui, session, path, before_failed_append);
    REQUIRE(ui.dispatch(application::UndoAction{}));
    check_event();
    REQUIRE(ui.dispatch(application::RedoAction{}));
    check_event();
    REQUIRE(session.snapshot().source_assets.front().authored_tags ==
        (std::vector<std::string>{"qa", "synthetic"}));

    const auto before_stale = capture_publication(ui, session, path);
    REQUIRE(!ui.apply_source_reimport_plan(first_review.value()));
    require_unchanged_publication(ui, session, path, before_stale);
    const auto reopened = project::ProjectDocument::load(path);
    REQUIRE(reopened && reopened.value().source_asset_registry().records().at(identity).authored_tags ==
        (std::vector<std::string>{"qa", "synthetic"}));
}

} // namespace

int main() {
    try {
        frontend_generates_lightmap_uv_for_exact_current_selection();
        frontend_refuses_lightmap_uv_when_another_locked_instance_shares_the_mesh();
        frontend_relinks_only_exact_moved_source_bytes_with_retained_lineage();
        frontend_restores_native_import_source_without_registry_or_journal_mutation();
        frontend_refuses_internal_source_symlink_substitution_after_relink_review();
        frontend_replays_import_and_metadata_history_with_retained_source_lineage();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
