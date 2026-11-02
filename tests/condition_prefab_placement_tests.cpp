// Fault injection is confined to this test translation unit. User operations
// below enter through UiController; private access supplies corrupt/stale
// source fixtures and a journal whose append is deliberately rejected.
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "application_access.hpp"
#undef private
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif

#include <carto/attributes/set.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/journal/journal.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ": " + #condition); } while (false)

using namespace carto;

struct TempProject {
    std::filesystem::path directory = std::filesystem::current_path() / "test-temp" /
        ("condition-prefab-placement-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempProject() { std::filesystem::create_directories(directory); }
    ~TempProject() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    std::filesystem::path path() const { return directory / "instance.carto"; }
};

struct Fixture {
    application::ApplicationSession session;
    ui::UiController ui{session};
    scene::ObjectId first;
    scene::ObjectId second;
    condition::Id prefab = 0U;

    Fixture() {
        REQUIRE(ui.dispatch(application::CreateBoxAction{"Source room", {4.0, 3.0, 5.0}}));
        REQUIRE(ui.dispatch(application::CreateBoxAction{"Source door", {0.8, 2.0, 0.1}}));
        const auto initial = ui.snapshot();
        first = initial.objects[0].object.id;
        second = initial.objects[1].object.id;
        core::Transform offset;
        offset.translation = {2.0, 0.0, 1.0};
        REQUIRE(ui.dispatch(application::SetObjectTransformAction{
            second, offset, initial.project_revision}));
        auto source = condition::make_condition_fixture();
        REQUIRE(source);
        for (auto& asset : source.value().assets) asset.authored_object_id = 0U;
        REQUIRE(ui.dispatch(application::ImportConditionAuthoringAction{source.value().serialize()}));
        auto state = ui.snapshot();
        REQUIRE(ui.bind_condition_asset_to_object(state.condition.assets[0].id, first,
            state.project_revision, state.condition.authored_revision));
        state = ui.snapshot();
        REQUIRE(ui.bind_condition_asset_to_object(state.condition.assets[2].id, second,
            state.project_revision, state.condition.authored_revision));
        state = ui.snapshot();
        condition::PrefabRecord definition;
        definition.name = "Room and door";
        definition.asset_ids = {state.condition.assets[0].id, state.condition.assets[2].id};
        definition.sockets = {"entry"};
        REQUIRE(ui.add_condition_prefab(definition, state.project_revision,
            state.condition.authored_revision));
        prefab = ui.snapshot().condition.prefabs.back().id;
    }

    core::Result<application::DispatchReceipt> place() {
        const auto state = ui.snapshot();
        return ui.instantiate_condition_prefab(prefab, "Isolated room", {40.0, 0.0, 80.0},
            state.project_revision, state.condition.authored_revision);
    }

    condition::PrefabInstanceRecord instance() const {
        const auto authoring = session.condition_authoring();
        REQUIRE(authoring && authoring.value().prefab_instances.size() == 1U);
        return authoring.value().prefab_instances.front();
    }

    geometry::EditableMesh& mesh(scene::ObjectId object) {
        const auto* source = session.document_.scene().find(object);
        REQUIRE(source != nullptr && source->mesh_asset.has_value());
        return const_cast<geometry::EditableMesh&>(session.document_.meshes().at(*source->mesh_asset));
    }
};

void attach_payload(geometry::EditableMesh& mesh, double coordinate = 0.25,
                    std::string uv_name = "Primary UV", double weight_value = 0.5) {
    attributes::AttributeSet payload;
    REQUIRE(payload.set_domain_count(attributes::AttributeDomain::vertex, mesh.vertex_count()));
    REQUIRE(payload.set_domain_count(attributes::AttributeDomain::face, mesh.face_count()));
    REQUIRE(payload.set_domain_count(attributes::AttributeDomain::corner, mesh.corners_sorted(100'001U).size()));
    attributes::AttributeLayer weight{{"fixture.weight", attributes::AttributeDomain::vertex,
        attributes::AttributeType::float64}, {}};
    weight.values.assign(mesh.vertex_count(), weight_value);
    REQUIRE(payload.add_layer(std::move(weight)));
    attributes::AttributeLayer region{{"condition.material_region", attributes::AttributeDomain::face,
        attributes::AttributeType::uint32}, {}};
    region.values.assign(mesh.face_count(), std::uint32_t{7U});
    REQUIRE(payload.add_layer(std::move(region)));
    attributes::AttributeLayer coordinates{{"fixture.uv", attributes::AttributeDomain::corner,
        attributes::AttributeType::vec2}, {}};
    coordinates.values.assign(mesh.corners_sorted(100'001U).size(), core::Vec2d{coordinate, 0.75});
    REQUIRE(payload.add_layer(std::move(coordinates)));
    attributes::UvSetTable uv;
    REQUIRE(uv.add({1U, std::move(uv_name), "fixture.uv", true, true}));
    REQUIRE(mesh.set_attribute_payload(std::move(payload), std::move(uv)));
}

void placement_isolated_geometry_undo_redo_and_save() {
    Fixture f;
    const auto before_meshes = f.session.document_.meshes().size();
    REQUIRE(f.place());
    const auto record = f.instance();
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
    REQUIRE(f.session.document_.meshes().size() == before_meshes + 2U);
    for (const auto& member : record.members) {
        REQUIRE(member.mesh_asset_id != member.instance_mesh_asset_id);
        const auto& source = f.session.document_.meshes().at(member.mesh_asset_id);
        const auto& clone = f.session.document_.meshes().at(member.instance_mesh_asset_id);
        REQUIRE(source.revision() == clone.revision());
        REQUIRE(member.source_snapshot_mesh_digest == member.snapshot_mesh_digest);
        REQUIRE(member.instance_mesh_revision == clone.revision());
        REQUIRE(project::canonical_mesh_snapshot_digest(source) == member.snapshot_mesh_digest);
        REQUIRE(project::canonical_mesh_snapshot_digest(clone) == member.snapshot_mesh_digest);
    }
    // An assembly placement moves the root without treating intact child-local
    // transforms as overrides.
    auto root_transform = f.session.document_.scene().find(
        scene::ObjectId{record.root_object_id})->local_transform;
    root_transform.translation.x += 12.0;
    REQUIRE(f.ui.dispatch(application::SetObjectTransformAction{
        scene::ObjectId{record.root_object_id}, root_transform, f.ui.snapshot().project_revision}));
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
    const auto& member = record.members.front();
    auto child_transform = f.session.document_.scene().find(
        scene::ObjectId{member.instance_object_id})->local_transform;
    child_transform.translation.y += 0.5;
    REQUIRE(f.ui.dispatch(application::SetObjectTransformAction{
        scene::ObjectId{member.instance_object_id}, child_transform, f.ui.snapshot().project_revision}));
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.meshes().size() == before_meshes);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.empty());
    REQUIRE(f.ui.dispatch(application::RedoAction{}));
    REQUIRE(f.session.document_.meshes().size() == before_meshes + 2U);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);

    TempProject temp;
    REQUIRE(f.ui.dispatch(application::SaveProjectAction{temp.path()}));
    application::ApplicationSession reopened;
    ui::UiController reopened_ui(reopened);
    REQUIRE(reopened_ui.dispatch(application::OpenProjectAction{temp.path()}));
    REQUIRE(reopened_ui.snapshot().condition.prefab_instances.front().current);
    for (const auto& item : record.members) {
        REQUIRE(project::canonical_mesh_snapshot_digest(reopened.document_.meshes().at(
            item.instance_mesh_asset_id)) == item.snapshot_mesh_digest);
    }

    // Exercise real human geometry commands in both directions. A source edit
    // makes the captured source stale, without modifying the isolated clone.
    const auto clone_before = project::canonical_mesh_snapshot_digest(
        f.session.document_.meshes().at(member.instance_mesh_asset_id));
    const auto source_geometry_before = application::canonical_mesh_geometry_digest(
        f.session.document_.meshes().at(member.mesh_asset_id));
    REQUIRE(f.ui.dispatch(application::SetSelectionModeAction{editor::SelectionMode::face}));
    REQUIRE(f.ui.dispatch(application::SelectFaceAction{
        scene::ObjectId{member.source_object_id}, geometry::FaceId{1U}}));
    editor::ToolArguments arguments;
    arguments.distance = 0.1;
    REQUIRE(f.ui.dispatch(application::InvokeToolAction{"mesh.extrude-face", arguments}));
    REQUIRE(project::canonical_mesh_snapshot_digest(f.session.document_.meshes().at(
        member.instance_mesh_asset_id)) == clone_before);
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(application::canonical_mesh_geometry_digest(f.session.document_.meshes().at(
        member.mesh_asset_id)) == source_geometry_before);
    // Undo restores content while advancing the mesh revision, so this exact
    // captured revision still requires an explicit rebase.
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    const auto source_before = project::canonical_mesh_snapshot_digest(
        f.session.document_.meshes().at(member.mesh_asset_id));
    REQUIRE(f.ui.dispatch(application::SelectFaceAction{
        scene::ObjectId{member.instance_object_id}, geometry::FaceId{1U}}));
    REQUIRE(f.ui.dispatch(application::InvokeToolAction{"mesh.extrude-face", arguments}));
    REQUIRE(project::canonical_mesh_snapshot_digest(f.session.document_.meshes().at(
        member.mesh_asset_id)) == source_before);
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
}

class CommaPunctuation final : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

void payload_digest_material_assignments_and_same_revision_drift() {
    Fixture f;
    attach_payload(f.mesh(f.first));
    REQUIRE(f.ui.dispatch(application::CreateMaterialAction{
        f.ui.snapshot().project_revision, "Room paint", {}}));
    const auto material = f.ui.snapshot().materials.back().id;
    const auto source_mesh = *f.session.document_.scene().find(f.first)->mesh_asset;
    auto catalog = f.session.document_.material_catalog();
    project::MaterialVariant unrestricted;
    unrestricted.id = 700U;
    unrestricted.name = "Unrestricted room finish";
    unrestricted.base_material = material;
    unrestricted.overrides.roughness = 0.6;
    unrestricted.revision = core::Revision{1U};
    REQUIRE(catalog.insert_variant(unrestricted));
    REQUIRE(f.session.document_.set_material_catalog(std::move(catalog)));
    REQUIRE(f.ui.dispatch(application::SetMaterialRegionAssignmentAction{
        f.ui.snapshot().project_revision, {source_mesh, 7U}, material, unrestricted.id}));
    REQUIRE(f.place());
    const auto record = f.instance();
    const auto member = *std::find_if(record.members.begin(), record.members.end(),
        [&](const auto& item) { return item.source_object_id == f.first.value; });
    const auto& clone = f.session.document_.meshes().at(member.instance_mesh_asset_id);
    REQUIRE(clone.attributes().layer_count() == 3U);
    REQUIRE(clone.uv_sets().find(1U) != nullptr);
    const auto& assignments = f.session.document_.material_catalog().assignments();
    const auto copied = assignments.find({member.instance_mesh_asset_id, 7U});
    REQUIRE(copied != assignments.end() && copied->second.material == material);
    REQUIRE(copied->second.variant == unrestricted.id);
    const auto baseline = project::canonical_mesh_snapshot_digest(f.mesh(f.first));
    const auto geometry_baseline = application::canonical_mesh_geometry_digest(f.mesh(f.first));
    const auto mesh_revision = f.mesh(f.first).revision();
    const auto old_locale = std::locale();
    std::locale::global(std::locale(old_locale, new CommaPunctuation));
    const auto locale_digest = project::canonical_mesh_snapshot_digest(f.mesh(f.first));
    std::locale::global(old_locale);
    REQUIRE(locale_digest == baseline);
    TempProject temp;
    REQUIRE(f.ui.dispatch(application::SaveProjectAction{temp.path()}));
    const auto saved = project::ProjectDocument::load(temp.path());
    REQUIRE(saved && project::canonical_mesh_snapshot_digest(saved.value().meshes().at(
        member.instance_mesh_asset_id)) == baseline);

    // Fault fixtures deliberately preserve the revision. Equality of revision
    // and geometry is insufficient evidence that an attributed source is current.
    attach_payload(f.mesh(f.first), 0.5);
    REQUIRE(f.mesh(f.first).restore_revision(mesh_revision));
    REQUIRE(application::canonical_mesh_geometry_digest(f.mesh(f.first)) == geometry_baseline);
    REQUIRE(project::canonical_mesh_snapshot_digest(f.mesh(f.first)) != baseline);
    REQUIRE(project::canonical_mesh_snapshot_digest(clone) == baseline);
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    attach_payload(f.mesh(f.first), 0.25, "Renamed UV");
    REQUIRE(f.mesh(f.first).restore_revision(mesh_revision));
    REQUIRE(application::canonical_mesh_geometry_digest(f.mesh(f.first)) == geometry_baseline);
    REQUIRE(project::canonical_mesh_snapshot_digest(f.mesh(f.first)) != baseline);
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    attach_payload(f.mesh(f.first), 0.25, "Primary UV", 0.75);
    REQUIRE(f.mesh(f.first).restore_revision(mesh_revision));
    REQUIRE(application::canonical_mesh_geometry_digest(f.mesh(f.first)) == geometry_baseline);
    REQUIRE(project::canonical_mesh_snapshot_digest(f.mesh(f.first)) != baseline);
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    attach_payload(f.mesh(f.first));
    REQUIRE(f.mesh(f.first).restore_revision(mesh_revision));
    REQUIRE(project::canonical_mesh_snapshot_digest(f.mesh(f.first)) == baseline);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);

    REQUIRE(f.ui.dispatch(application::CreateMaterialAction{
        f.ui.snapshot().project_revision, "Updated source paint", {}}));
    const auto updated_material = f.ui.snapshot().materials.back().id;
    REQUIRE(f.ui.dispatch(application::SetMaterialRegionAssignmentAction{
        f.ui.snapshot().project_revision, {source_mesh, 7U}, updated_material, std::nullopt}));
    const auto& updated_assignments = f.session.document_.material_catalog().assignments();
    REQUIRE(updated_assignments.at({source_mesh, 7U}).material == updated_material);
    REQUIRE(updated_assignments.at({member.instance_mesh_asset_id, 7U}).material == material);
    REQUIRE(updated_assignments.at({member.instance_mesh_asset_id, 7U}).variant == unrestricted.id);
    REQUIRE(project::canonical_mesh_snapshot_digest(f.session.document_.meshes().at(
        member.instance_mesh_asset_id)) == baseline);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);

    auto legacy = f.session.condition_authoring();
    REQUIRE(legacy);
    for (auto& item : legacy.value().prefab_instances.front().members) {
        item.instance_mesh_asset_id = 0U;
        item.snapshot_mesh_digest = {};
        item.source_snapshot_mesh_digest = {};
        item.instance_mesh_revision = {};
        item.instance_local_transform = core::Transform::identity();
    }
    const auto digest = legacy.value().canonical_digest();
    REQUIRE(digest);
    REQUIRE(f.session.document_.set_condition_authoring(legacy.value().serialize(), digest.value()));
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    REQUIRE(!f.ui.build_condition_package());
    REQUIRE(project::canonical_mesh_snapshot_digest(f.mesh(f.first)) == baseline);
}

void journal_failure_and_ai_origin_are_atomic() {
    Fixture f;
    auto moved = f.session.document_.scene().find(f.first)->local_transform;
    moved.translation.y = 1.0;
    REQUIRE(f.ui.dispatch(application::SetObjectTransformAction{
        f.first, moved, f.ui.snapshot().project_revision}));
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.ui.snapshot().redo_count == 1U);
    TempProject temp;
    REQUIRE(f.ui.dispatch(application::SaveProjectAction{temp.path()}));
    journal::Journal events(std::filesystem::path(temp.path().string() + ".journal"));
    const auto initial_events = events.read_all();
    REQUIRE(initial_events);
    const auto before = f.session.document_.serialize();
    const auto state = f.ui.snapshot();
    const application::InstantiateConditionPrefabAction action{
        state.project_revision, state.condition.authored_revision, f.prefab,
        "AI must not place", {1.0, 0.0, 2.0}};
    REQUIRE(!f.ui.submit_ai_proposal(action));
    REQUIRE(f.session.document_.serialize() == before);
    journal::JournalLimits limits;
    limits.max_payload_bytes = 1U;
    f.session.journal_.emplace(events.path(), limits);
    REQUIRE(!f.place());
    REQUIRE(f.session.document_.serialize() == before);
    const auto failed = f.ui.snapshot();
    REQUIRE(failed.undo_count == state.undo_count && failed.redo_count == state.redo_count);
    REQUIRE(failed.operations.size() == state.operations.size());
    REQUIRE(failed.condition.prefab_instances.empty());
    REQUIRE(events.read_all().value().size() == initial_events.value().size());
    f.session.journal_.emplace(events.path());
    const auto accepted = f.place();
    REQUIRE(accepted && accepted.value().source == application::OperationSource::human);
    REQUIRE(f.ui.snapshot().undo_count == state.undo_count + 1U);
    REQUIRE(f.ui.snapshot().redo_count == 0U);
    REQUIRE(events.read_all().value().size() == initial_events.value().size() + 1U);
    REQUIRE(f.session.document_.meshes().size() == 4U);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
}

void late_member_and_variant_failures_publish_no_partial_clones() {
    Fixture f;
    // The first member resolves; the second member becomes unavailable.
    REQUIRE(f.session.document_.scene_.remove_object(f.second));
    const auto before = f.session.document_.serialize();
    const auto state = f.ui.snapshot();
    REQUIRE(!f.place());
    REQUIRE(f.session.document_.serialize() == before);
    REQUIRE(f.ui.snapshot().undo_count == state.undo_count);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.empty());

    Fixture restricted;
    attach_payload(restricted.mesh(restricted.second));
    REQUIRE(restricted.ui.dispatch(application::CreateMaterialAction{
        restricted.ui.snapshot().project_revision, "Restricted door finish", {}}));
    const auto material = restricted.ui.snapshot().materials.back().id;
    const auto source_mesh = *restricted.session.document_.scene().find(restricted.second)->mesh_asset;
    auto catalog = restricted.session.document_.material_catalog();
    project::MaterialVariant variant;
    variant.id = 900U;
    variant.name = "Source-only finish";
    variant.base_material = material;
    variant.overrides.roughness = 0.8;
    variant.permitted_mesh_assets = {source_mesh};
    variant.revision = core::Revision{1U};
    REQUIRE(catalog.insert_variant(variant));
    REQUIRE(catalog.insert_assignment({{source_mesh, 7U}, material, variant.id}));
    REQUIRE(restricted.session.document_.set_material_catalog(std::move(catalog)));
    const auto restricted_before = restricted.session.document_.serialize();
    const auto restricted_state = restricted.ui.snapshot();
    REQUIRE(!restricted.place());
    REQUIRE(restricted.session.document_.serialize() == restricted_before);
    REQUIRE(restricted.ui.snapshot().undo_count == restricted_state.undo_count);
    REQUIRE(restricted.ui.snapshot().operations.size() == restricted_state.operations.size());
    REQUIRE(restricted.ui.snapshot().condition.prefab_instances.empty());
}

void aggregate_snapshot_budget_refuses_individually_bounded_members() {
    Fixture f;
    for (const auto object : {f.first, f.second}) {
        auto& mesh = f.mesh(object);
        auto vertices = mesh.vertices_sorted();
        const auto faces = mesh.faces_sorted();
        for (std::uint64_t id = vertices.size() + 1U; id <= 10'000U; ++id) {
            vertices.push_back({geometry::VertexId{id}, {static_cast<double>(id), 10.0, 0.0}});
        }
        geometry::EditableMesh bounded;
        REQUIRE(bounded.insert_bulk(std::move(vertices), faces));
        attributes::AttributeSet payload;
        REQUIRE(payload.set_domain_count(attributes::AttributeDomain::vertex, bounded.vertex_count()));
        for (unsigned int index = 0U; index < 5U; ++index) {
            attributes::AttributeLayer layer{{"fixture.bound." + std::to_string(index),
                attributes::AttributeDomain::vertex, attributes::AttributeType::float32}, {}};
            layer.values.assign(bounded.vertex_count(), 0.5F);
            REQUIRE(payload.add_layer(std::move(layer)));
        }
        REQUIRE(bounded.set_attribute_payload(std::move(payload), {}));
        mesh = std::move(bounded);
    }
    const auto before = f.session.document_.serialize();
    const auto state = f.ui.snapshot();
    REQUIRE(!f.place());
    REQUIRE(f.session.document_.serialize() == before);
    REQUIRE(f.ui.snapshot().operations.size() == state.operations.size());
    REQUIRE(f.ui.snapshot().undo_count == state.undo_count);
    REQUIRE(f.session.document_.meshes().size() == 2U);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.empty());
}

void portable_sculpt_authoring_refuses_lossy_placement() {
    Fixture f;
    const auto mesh_id = *f.session.document_.scene().find(f.second)->mesh_asset;
    const auto faces = f.mesh(f.second).faces_sorted();
    REQUIRE(!faces.empty() && faces.front().vertices.size() >= 3U);
    const production::SculptSurfaceAnchor anchor{
        faces.front().id,
        {faces.front().vertices[0], faces.front().vertices[1], faces.front().vertices[2]},
        {0.25, 0.25, 0.5}};
    const auto sample = production::make_sculpt_surface_sample(f.mesh(f.second), anchor);
    REQUIRE(sample);
    const auto sculpt_snapshot = f.ui.snapshot();
    REQUIRE(f.ui.dispatch(application::AppendSculptSurfaceStrokeAction{
        sculpt_snapshot.project_revision, f.second, mesh_id,
        production::SculptStrokeMode::add, 0.75, 0.05, {sample.value()},
        sculpt_snapshot.project_generation}));
    REQUIRE(f.session.document_.sculpt_stacks().contains(mesh_id));
    REQUIRE(production::SculptStack::deserialize(
        f.session.document_.sculpt_stacks().at(mesh_id).bytes));
    REQUIRE(f.session.document_.validate());
    const auto before = f.session.document_.serialize();
    const auto state = f.ui.snapshot();
    REQUIRE(!f.place());
    REQUIRE(f.session.document_.serialize() == before);
    REQUIRE(f.ui.snapshot().undo_count == state.undo_count);
    REQUIRE(f.ui.snapshot().operations.size() == state.operations.size());
    REQUIRE(f.session.document_.meshes().size() == 2U);
    REQUIRE(f.ui.snapshot().condition.prefab_instances.empty());
}

void snapshot_alias_owner_invalidates_isolation_without_changing_bytes() {
    Fixture f;
    REQUIRE(f.place());
    const auto member = f.instance().members.front();
    const auto digest = project::canonical_mesh_snapshot_digest(
        f.session.document_.meshes().at(member.instance_mesh_asset_id));
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
    // The normal duplicate command refuses child objects. Inject a second
    // owner without a project revision change to test imported/corrupt state,
    // rather than widening the human command's scope for this fixture.
    const auto before_alias = f.session.document_;
    const auto alias = f.session.document_.scene_.create_object("Aliased snapshot");
    REQUIRE(alias);
    REQUIRE(f.session.document_.scene_.attach_mesh(alias.value(), member.instance_mesh_asset_id));
    REQUIRE(project::canonical_mesh_snapshot_digest(f.session.document_.meshes().at(
        member.instance_mesh_asset_id)) == digest);
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    REQUIRE(!f.ui.snapshot().condition.publication_ready);
    REQUIRE(!f.ui.build_condition_package());
    f.session.document_ = before_alias;
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
}

void prefab_rebase_review_is_wired_to_application_snapshot() {
    Fixture f;
    REQUIRE(f.place());
    const auto original_instance = f.instance();
    auto state = f.ui.snapshot();
    REQUIRE(state.condition.prefab_instances.size() == 1U);
    const auto& current = state.condition.prefab_instances.front();
    REQUIRE(current.rebase_review_available);
    REQUIRE(current.rebase_change_count == 0U);
    REQUIRE(current.rebase_status.find("Project mesh and local override review still required") !=
        std::string::npos);

    auto authoring = f.session.condition_authoring();
    REQUIRE(authoring && !authoring.value().prefabs.empty());
    const auto selected_prefab = std::find_if(authoring.value().prefabs.begin(),
        authoring.value().prefabs.end(), [&f](const auto& prefab) { return prefab.id == f.prefab; });
    REQUIRE(selected_prefab != authoring.value().prefabs.end());
    auto changed_prefab = *selected_prefab;
    REQUIRE(changed_prefab.asset_ids.size() == 2U);
    const condition::Id removed_asset_id = changed_prefab.asset_ids.back();
    changed_prefab.asset_ids.pop_back();
    REQUIRE(f.ui.replace_condition_authored_record(changed_prefab,
        state.project_revision, state.condition.authored_revision));

    const auto reviewed_state = f.ui.snapshot();
    const auto& reviewed = reviewed_state.condition.prefab_instances.front();
    REQUIRE(!reviewed.current);
    REQUIRE(reviewed.rebase_review_available);
    REQUIRE(reviewed.rebase_change_count == 1U);
    REQUIRE(reviewed.rebase_changes.front().find("Member removed") != std::string::npos);
    REQUIRE(reviewed.rebase_status.find("membership choices still require review") !=
        std::string::npos);

    const auto removed_member = std::lower_bound(original_instance.members.begin(),
        original_instance.members.end(), removed_asset_id,
        [](const auto& item, condition::Id id) { return item.asset_id < id; });
    REQUIRE(removed_member != original_instance.members.end() &&
        removed_member->asset_id == removed_asset_id);
    const auto removed_object_id = scene::ObjectId{removed_member->instance_object_id};
    const condition::Id retained_asset_id = changed_prefab.asset_ids.front();
    const auto retained_member = std::lower_bound(original_instance.members.begin(),
        original_instance.members.end(), retained_asset_id,
        [](const auto& item, condition::Id id) { return item.asset_id < id; });
    REQUIRE(retained_member != original_instance.members.end() &&
        retained_member->asset_id == retained_asset_id);
    const auto removed_world_before = f.session.document_.scene().world_transform(removed_object_id);
    REQUIRE(removed_world_before);

    application::RebaseConditionPrefabAction action;
    action.expected_project_generation = reviewed_state.project_generation;
    action.expected_project_revision = reviewed_state.project_revision;
    action.expected_authoring_revision = reviewed_state.condition.authored_revision;
    action.expected_authoring_digest = reviewed.rebase_authoring_digest;
    action.instance_id = original_instance.id;
    action.justification = "Refresh retained geometry and preserve its placement.";
    application::ConditionPrefabRebaseDecision retained;
    retained.asset_id = retained_asset_id;
    retained.mesh = application::ConditionPrefabRebaseMeshChoice::use_current_source_snapshot;
    retained.placement = application::ConditionPrefabRebasePlacementChoice::keep_instance_placement;
    application::ConditionPrefabRebaseDecision removed;
    removed.asset_id = removed_asset_id;
    removed.removed_member = application::ConditionPrefabRemovedMemberChoice::detach_scene_object;
    action.decisions = {retained, removed};
    std::sort(action.decisions.begin(), action.decisions.end(),
        [](const auto& left, const auto& right) { return left.asset_id < right.asset_id; });

    auto missing_justification = action;
    missing_justification.justification.clear();
    REQUIRE(!f.ui.rebase_condition_prefab(std::move(missing_justification)));
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    auto unchanged_authoring = f.session.condition_authoring();
    REQUIRE(unchanged_authoring && unchanged_authoring.value().prefab_rebase_receipts.empty());
    const auto rebase_receipt = f.ui.rebase_condition_prefab(std::move(action));
    REQUIRE(rebase_receipt);
    REQUIRE(std::binary_search(rebase_receipt.value().affected.mesh_assets.begin(),
        rebase_receipt.value().affected.mesh_assets.end(), removed_member->instance_mesh_asset_id));
    REQUIRE(std::binary_search(rebase_receipt.value().affected.mesh_assets.begin(),
        rebase_receipt.value().affected.mesh_assets.end(), retained_member->instance_mesh_asset_id));

    auto rebased = f.ui.snapshot();
    REQUIRE(rebased.condition.prefab_instances.front().current);
    REQUIRE(rebased.condition.prefab_instances.front().member_count == 1U);
    REQUIRE(application::canonical_mesh_geometry_digest(
        f.session.document_.meshes().at(retained_member->mesh_asset_id)) ==
        application::canonical_mesh_geometry_digest(
            f.session.document_.meshes().at(retained_member->instance_mesh_asset_id)));
    const auto* detached = f.session.document_.scene().find(removed_object_id);
    REQUIRE(detached != nullptr && !detached->parent.has_value());
    REQUIRE(detached->mesh_asset == removed_member->instance_mesh_asset_id);
    const auto removed_world_after = f.session.document_.scene().world_transform(removed_object_id);
    REQUIRE(removed_world_after &&
        removed_world_after.value().translation.x == removed_world_before.value().translation.x &&
        removed_world_after.value().translation.y == removed_world_before.value().translation.y &&
        removed_world_after.value().translation.z == removed_world_before.value().translation.z);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(!f.ui.snapshot().condition.prefab_instances.front().current);
    auto undone_authoring = f.session.condition_authoring();
    REQUIRE(undone_authoring && undone_authoring.value().prefab_rebase_receipts.empty());
    REQUIRE(f.ui.dispatch(application::RedoAction{}));
    REQUIRE(f.ui.snapshot().condition.prefab_instances.front().current);
    auto redone_authoring = f.session.condition_authoring();
    REQUIRE(redone_authoring && redone_authoring.value().prefab_rebase_receipts.size() == 1U);
    const auto& receipt = redone_authoring.value().prefab_rebase_receipts.front();
    REQUIRE(receipt.justification == "Refresh retained geometry and preserve its placement.");
    REQUIRE(receipt.reviewed_authoring_digest == reviewed.rebase_authoring_digest);
    REQUIRE(receipt.decisions.size() == 2U);
    const auto retained_choice = std::lower_bound(receipt.decisions.begin(), receipt.decisions.end(),
        retained_asset_id, [](const auto& choice, condition::Id id) {
            return choice.asset_id < id;
        });
    REQUIRE(retained_choice != receipt.decisions.end() && retained_choice->asset_id == retained_asset_id);
    REQUIRE(retained_choice->mesh == condition::PrefabRebaseMeshDecision::use_current_source_snapshot);
    const auto removed_choice = std::lower_bound(receipt.decisions.begin(), receipt.decisions.end(),
        removed_asset_id, [](const auto& choice, condition::Id id) {
            return choice.asset_id < id;
        });
    REQUIRE(removed_choice != receipt.decisions.end() && removed_choice->asset_id == removed_asset_id);
    REQUIRE(removed_choice->membership == condition::PrefabRebaseMembershipDecision::detached);
}

void prefab_socket_connections_are_revision_bound_persisted_and_undoable() {
    Fixture f;
    TempProject temp;
    REQUIRE(f.ui.dispatch(application::SaveProjectAction{temp.path()}));
    journal::Journal events(std::filesystem::path(temp.path().string() + ".journal"));
    REQUIRE(f.place());
    const auto first_place = f.session.condition_authoring();
    REQUIRE(first_place && first_place.value().prefab_instances.size() == 1U);
    REQUIRE(f.place());
    auto authoring = f.session.condition_authoring();
    REQUIRE(authoring && authoring.value().prefab_instances.size() == 2U);

    connection::ConnectionPort source_port;
    source_port.owner = f.first;
    source_port.authoring.kind = world::AuthoringBindingKind::scene_object;
    source_port.authoring.identity = f.first.value;
    source_port.local_frame.frame_identity = "entry";
    source_port.local_frame.coordinate_convention = "carto.core/v1";
    source_port.compatibility_tags = {
        connection::compatibility::mechanical_hitch::pin_hitch_19mm};
    auto state = f.ui.snapshot();
    const auto graph_digest = f.session.document_.connection_port_graph().canonical_digest();
    REQUIRE(graph_digest);
    application::AddConnectionPortAction add_source_port;
    add_source_port.expected_project_generation = state.project_generation;
    add_source_port.expected_project_revision = state.project_revision;
    add_source_port.expected_graph_digest = graph_digest.value();
    add_source_port.port = source_port;
    REQUIRE(f.ui.add_connection_port(std::move(add_source_port)));
    REQUIRE(f.session.document_.connection_port_graph().find_port(connection::PortId{1U}) != nullptr);

    state = f.ui.snapshot();
    application::ReplaceConnectionPortGraphAction stale_graph_edit;
    stale_graph_edit.expected_project_generation = state.project_generation;
    stale_graph_edit.expected_project_revision = state.project_revision;
    stale_graph_edit.expected_graph_digest = graph_digest.value();
    stale_graph_edit.graph = connection::ConnectionPortGraph{};
    REQUIRE(!f.ui.replace_connection_port_graph(std::move(stale_graph_edit)));
    REQUIRE(state.condition.connection_port_count == 1U);
    REQUIRE(state.condition.connection_edge_count == 0U);
    application::AddConnectionPortAction stale_port_add;
    stale_port_add.expected_project_generation = state.project_generation;
    stale_port_add.expected_project_revision = state.project_revision;
    stale_port_add.expected_graph_digest = graph_digest.value();
    stale_port_add.port = source_port;
    stale_port_add.port.local_frame.frame_identity = "exit";
    REQUIRE(!f.ui.add_connection_port(std::move(stale_port_add)));
    REQUIRE(f.ui.snapshot().condition.connection_port_count == 1U);
    authoring = f.session.condition_authoring();
    REQUIRE(authoring);
    const auto authoring_digest = authoring.value().canonical_digest();
    REQUIRE(authoring_digest);
    const auto current_graph_digest =
        f.session.document_.connection_port_graph().canonical_digest();
    REQUIRE(current_graph_digest);
    application::ConnectConditionPrefabSocketsAction connect;
    connect.expected_project_generation = state.project_generation;
    connect.expected_project_revision = state.project_revision;
    connect.expected_graph_digest = current_graph_digest.value();
    connect.expected_authoring_revision = authoring.value().project_revision;
    connect.expected_authoring_digest = authoring_digest.value();
    connect.first_instance_id = authoring.value().prefab_instances[0].id;
    connect.first_socket = "entry";
    connect.second_instance_id = authoring.value().prefab_instances[1].id;
    connect.second_socket = "entry";
    connect.kind = connection::ConnectionKind::mechanical;
    auto same_instance = connect;
    same_instance.second_instance_id = same_instance.first_instance_id;
    REQUIRE(!f.ui.connect_condition_prefab_sockets(std::move(same_instance)));
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 1U);
    REQUIRE(f.session.document_.connection_port_graph().edges().empty());
    auto stale_connection = connect;
    stale_connection.expected_graph_digest = graph_digest.value();
    REQUIRE(!f.ui.connect_condition_prefab_sockets(std::move(stale_connection)));
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 1U);
    REQUIRE(f.ui.connect_condition_prefab_sockets(std::move(connect)));
    state = f.ui.snapshot();
    REQUIRE(state.condition.connection_port_count == 3U);
    REQUIRE(state.condition.connection_edge_count == 1U);

    const auto& graph = f.session.document_.connection_port_graph();
    REQUIRE(graph.ports().size() == 3U);
    REQUIRE(graph.edges().size() == 1U);
    const auto& edge = graph.edges().begin()->second;
    REQUIRE(edge.evidence.validate());
    const auto* first_instance_port = graph.find_port(edge.first_port);
    const auto* second_instance_port = graph.find_port(edge.second_port);
    REQUIRE(first_instance_port != nullptr && second_instance_port != nullptr);
    REQUIRE(first_instance_port->owner != second_instance_port->owner);
    REQUIRE(first_instance_port->local_frame.frame_identity == "entry");
    REQUIRE(second_instance_port->local_frame.frame_identity == "entry");
    const auto connected_edge_id = edge.id;
    const auto connected_first_port = edge.first_port;
    const auto connected_second_port = edge.second_port;
    const auto connected_port_id = first_instance_port->id;
    const auto reopened = project::ProjectDocument::deserialize(
        f.session.document_.serialize());
    REQUIRE(reopened && reopened.value().connection_port_graph().edges().size() == 1U);
    auto mislabeled_schema33 = f.session.document_.serialize();
    const auto mislabeled_schema33_drafts = mislabeled_schema33.find("UNADMITTED_MODE_KEY_DRAFTS 0\n");
    REQUIRE(mislabeled_schema33_drafts != std::string::npos);
    mislabeled_schema33.erase(mislabeled_schema33_drafts, std::string("UNADMITTED_MODE_KEY_DRAFTS 0\n").size());
    if (const auto control_workspace = mislabeled_schema33.find("CONTROL_WORKSPACE 0\n"); control_workspace != std::string::npos)
        mislabeled_schema33.erase(control_workspace, std::string("CONTROL_WORKSPACE 0\n").size());
    mislabeled_schema33.replace(0U,
        std::string("CARTOGRAPHER_PROJECT 39\n").size(), "CARTOGRAPHER_PROJECT 33\n");
    const std::string accepted_table = "ACCEPTED_AUTORIGS 0\n";
    const auto accepted_pos = mislabeled_schema33.find(accepted_table);
    REQUIRE(accepted_pos != std::string::npos);
    mislabeled_schema33.erase(accepted_pos, accepted_table.size());
    const auto kinematic_table = mislabeled_schema33.find("KINEMATIC_GRAPHS 0\n");
    REQUIRE(kinematic_table != std::string::npos);
    mislabeled_schema33.erase(kinematic_table,
        std::string("KINEMATIC_GRAPHS 0\n").size());
    REQUIRE(!project::ProjectDocument::deserialize(mislabeled_schema33));

    const auto connected_digest =
        f.session.document_.connection_port_graph().canonical_digest();
    REQUIRE(connected_digest);
    state = f.ui.snapshot();
    const auto before_forgery = f.session.document_.serialize();
    const auto before_forgery_events = events.read_all();
    REQUIRE(before_forgery_events);
    auto forged_graph = f.session.document_.connection_port_graph();
    // Fault injection only: a fully formed edge must not grant authority
    // through a structurally valid but fabricated compatibility receipt.
    const_cast<connection::PortConnectionEdge&>(forged_graph.edges().begin()->second).
        evidence.first_satisfied_requirements = {"power-transfer"};
    application::ReplaceConnectionPortGraphAction forged_replacement;
    forged_replacement.expected_project_generation = state.project_generation;
    forged_replacement.expected_project_revision = state.project_revision;
    forged_replacement.expected_graph_digest = connected_digest.value();
    forged_replacement.graph = std::move(forged_graph);
    REQUIRE(!f.ui.replace_connection_port_graph(std::move(forged_replacement)));
    REQUIRE(f.session.document_.serialize() == before_forgery);
    REQUIRE(f.ui.snapshot().undo_count == state.undo_count &&
        f.ui.snapshot().redo_count == state.redo_count);
    REQUIRE(events.read_all().value().size() == before_forgery_events.value().size() && events.verify());
    auto forged_project = f.session.document_;
    const_cast<connection::PortConnectionEdge&>(forged_project.connection_port_graph().edges().begin()->second).
        evidence.first_satisfied_requirements = {"power-transfer"};
    REQUIRE(!project::ProjectDocument::deserialize(forged_project.serialize()));
    application::RemoveConnectionPortAction connected_port_removal;
    connected_port_removal.expected_project_generation = state.project_generation;
    connected_port_removal.expected_project_revision = state.project_revision;
    connected_port_removal.expected_graph_digest = connected_digest.value();
    connected_port_removal.port_id = connected_port_id;
    REQUIRE(!f.ui.remove_connection_port(std::move(connected_port_removal)));
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 3U);
    application::RemoveConnectionEdgeAction remove_edge;
    remove_edge.expected_project_generation = state.project_generation;
    remove_edge.expected_project_revision = state.project_revision;
    remove_edge.expected_graph_digest = connected_digest.value();
    remove_edge.edge_id = connected_edge_id;
    const auto before_remove = f.session.document_.serialize();
    const auto before_remove_events = events.read_all();
    REQUIRE(before_remove_events);
    REQUIRE(!f.ui.submit_ai_proposal(remove_edge));
    journal::JournalLimits limits;
    limits.max_payload_bytes = 1U;
    f.session.journal_.emplace(events.path(), limits);
    REQUIRE(!f.ui.remove_connection_edge(remove_edge));
    REQUIRE(f.session.document_.serialize() == before_remove);
    REQUIRE(f.ui.snapshot().undo_count == state.undo_count);
    REQUIRE(events.read_all().value().size() == before_remove_events.value().size());
    f.session.journal_.emplace(events.path());
    REQUIRE(f.ui.remove_connection_edge(std::move(remove_edge)));
    const auto removed_events = events.read_all();
    REQUIRE(removed_events && removed_events.value().size() == before_remove_events.value().size() + 1U);
    REQUIRE(removed_events.value().back().event_type == "cartographer.application_action");
    const std::string removal_payload(removed_events.value().back().payload.begin(),
        removed_events.value().back().payload.end());
    REQUIRE(removal_payload.find("NEXT_EDGE_ID 2") != std::string::npos && events.verify());
    REQUIRE(f.session.document_.connection_port_graph().edges().empty());
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 3U);
    const auto disconnected_document = project::ProjectDocument::deserialize(
        f.session.document_.serialize());
    REQUIRE(disconnected_document);
    auto reopened_graph = disconnected_document.value().connection_port_graph();
    const auto reconnected = reopened_graph.connect(connected_first_port, connected_second_port,
        connection::ConnectionKind::mechanical);
    REQUIRE(reconnected && reconnected.value().id > connected_edge_id);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().edges().size() == 1U);
    REQUIRE(f.ui.dispatch(application::RedoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().edges().empty());
    REQUIRE(events.read_all().value().size() == removed_events.value().size() + 2U && events.verify());
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().edges().size() == 1U);

    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().edges().empty());
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 1U);
    REQUIRE(f.ui.dispatch(application::RedoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().edges().size() == 1U);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 1U);
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().ports().empty());
    REQUIRE(f.ui.dispatch(application::RedoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 1U);
    state = f.ui.snapshot();
    const auto source_only_digest =
        f.session.document_.connection_port_graph().canonical_digest();
    REQUIRE(source_only_digest);
    application::RemoveConnectionPortAction remove_source_port;
    remove_source_port.expected_project_generation = state.project_generation;
    remove_source_port.expected_project_revision = state.project_revision;
    remove_source_port.expected_graph_digest = source_only_digest.value();
    remove_source_port.port_id = connection::PortId{1U};
    REQUIRE(f.ui.remove_connection_port(std::move(remove_source_port)));
    REQUIRE(f.session.document_.connection_port_graph().ports().empty());
    REQUIRE(f.ui.dispatch(application::UndoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().ports().size() == 1U);
    REQUIRE(f.ui.dispatch(application::RedoAction{}));
    REQUIRE(f.session.document_.connection_port_graph().ports().empty());
}

} // namespace

int main() {
    try {
        placement_isolated_geometry_undo_redo_and_save();
        payload_digest_material_assignments_and_same_revision_drift();
        journal_failure_and_ai_origin_are_atomic();
        late_member_and_variant_failures_publish_no_partial_clones();
        aggregate_snapshot_budget_refuses_individually_bounded_members();
        portable_sculpt_authoring_refuses_lossy_placement();
        snapshot_alias_owner_invalidates_isolation_without_changing_bytes();
        prefab_rebase_review_is_wired_to_application_snapshot();
        prefab_socket_connections_are_revision_bound_persisted_and_undoable();
        std::cout << "CONDITION isolated prefab placement acceptance passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
