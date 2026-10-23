#include <carto/application/application.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/assets/source_registry.hpp>
#include <carto/attributes/set.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/io/obj.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/project/presentation_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/render/material.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

assets::Sha256Digest digest(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "synthetic pickup source could not be reopened");
    return std::string{std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

geometry::EditableMesh box_with_material_region(
    core::Vec3d dimensions, std::uint32_t region_id) {
    auto mesh = geometry::make_box(dimensions);
    require(static_cast<bool>(mesh), "pickup blockout box geometry was rejected");
    const auto faces = mesh.value().faces_sorted();
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::face, faces.size()}});
    std::vector<attributes::AttributeValue> regions(
        faces.size(), region_id);
    require(static_cast<bool>(payload.add_layer(attributes::AttributeLayer{
                {"cartographer.material_slot", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32}, std::move(regions)})),
        "pickup material-region source could not be authored");
    require(static_cast<bool>(mesh.value().set_attribute_payload(std::move(payload), {})),
        "pickup material-region payload does not match its mesh");
    return std::move(mesh.value());
}

struct Part final {
    scene::ObjectId object;
    std::uint64_t mesh_asset = 0U;
    core::Revision mesh_revision;
    core::Vec3d position{};
};

geometry::EditableMesh marker_mesh() {
    auto mesh = geometry::make_box({0.08, 0.08, 0.08});
    require(static_cast<bool>(mesh), "pickup review marker geometry was rejected");
    return std::move(mesh.value());
}

Part add_part(
    ui::UiController& controller,
    std::string name,
    geometry::EditableMesh mesh,
    core::Vec3d position) {
    require(static_cast<bool>(controller.dispatch(application::CreateMeshObjectAction{
                name, std::move(mesh)})),
        "pickup component mesh could not be admitted");
    auto snapshot = controller.snapshot();
    const auto found = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [&name](const auto& item) { return item.object.name == name; });
    require(found != snapshot.objects.end() && found->object.mesh_asset.has_value() &&
            found->mesh_revision.has_value(),
        "pickup component has no project-owned revisioned mesh");
    const auto object_id = found->object.id;
    core::Transform transform = core::Transform::identity();
    transform.translation = position;
    require(static_cast<bool>(controller.dispatch(application::SetObjectTransformAction{
                object_id, transform, snapshot.project_revision})),
        "pickup component transform did not use the captured project revision");
    snapshot = controller.snapshot();
    const auto transformed = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
        [object_id](const auto& item) { return item.object.id == object_id; });
    require(transformed != snapshot.objects.end() && transformed->mesh_revision.has_value(),
        "pickup component disappeared after its transform command");
    return Part{object_id, *transformed->object.mesh_asset,
        *transformed->mesh_revision, position};
}

assets::SourceImportRecord source_record_for(
    const std::filesystem::path& project_path,
    const assets::BlobStore& store,
    const assets::BlobRef& blob,
    assets::Sha256Digest geometry_digest,
    core::Revision generation) {
    assets::SourceImportRecord record;
    record.identity = "condition.pickup.source";
    record.relative_source_path = store.path_for(blob.digest)
        .lexically_relative(project_path.parent_path()).generic_string();
    record.source_generation = generation;
    record.kind = assets::SourceAssetKind::mesh;
    record.source_digest = blob.digest;
    record.importer_identity = "carto.import.obj";
    record.importer_version = 1U;
    record.artifact_identity = "condition.pickup.chassis.mesh";
    record.artifact_digest = geometry_digest;
    record.domain_digests = {{assets::SourceAssetDomain::geometry, geometry_digest}};
    require(static_cast<bool>(record.validate()),
        "pickup source-import record did not validate");
    return record;
}

condition::AuthoringDocument pickup_authoring(
    const assets::SourceImportRecord& source,
    const assets::Sha256Digest& material_digest) {
    condition::AuthoringDocument authoring;
    authoring.project_namespace = "condition.fixture.pickup-truck";
    authoring.project_revision = core::Revision{1U};
    authoring.sources.push_back(condition::SourceEvidence{
        1U,
        condition::SourceKind::imported_asset,
        "Synthetic pickup blockout source",
        "fixtures/pickup-truck/source.obj",
        "1",
        source.source_digest,
        "cartographer.synthetic.obj-fixture",
        "1",
        source.options.digest(),
        true,
        true,
        {"synthetic coarse blockout; not production vehicle geometry"}});
    authoring.assets.push_back(condition::SourceAsset{
        2U,
        "Pickup truck assembly",
        condition::AssetClass::vehicle,
        condition::FidelityClass::familiarity_critical,
        1U,
        "1",
        source.source_digest,
        source.artifact_digest,
        material_digest,
        0U,
        {"pickup", "synthetic"},
        {"cab", "doors", "payload-bed", "wheels"}});

    condition::VariantCandidate loaded_bed;
    loaded_bed.id = 3U;
    loaded_bed.asset_id = 2U;
    loaded_bed.name = "Loaded utility bed";
    loaded_bed.kind = condition::VariantKind::morph;
    loaded_bed.base_revision = authoring.project_revision;
    loaded_bed.maximum_transform_delta = 0.25;
    loaded_bed.collision_impact_declared = true;
    loaded_bed.navigation_impact_declared = true;
    loaded_bed.interaction_impact_declared = true;
    loaded_bed.semantic_id = "vehicle.pickup.payload.loaded";
    authoring.variants.push_back(loaded_bed);

    condition::VariantCandidate night_finish;
    night_finish.id = 4U;
    night_finish.asset_id = 2U;
    night_finish.name = "Night review finish";
    night_finish.kind = condition::VariantKind::presentation;
    night_finish.base_revision = authoring.project_revision;
    night_finish.projection_only = true;
    night_finish.semantic_id = "vehicle.pickup.presentation.night";
    authoring.variants.push_back(night_finish);

    condition::SurfaceStateRecord surface_state;
    surface_state.id = 5U;
    surface_state.asset_id = 2U;
    surface_state.occupancy = condition::OccupancyState::recently_used;
    surface_state.maintenance = condition::MaintenanceState::aging;
    surface_state.damage = condition::DamageState::intact;
    surface_state.wetness = 0.12;
    surface_state.wear_tags = {"dusty-paint"};
    surface_state.provenance_note = "Synthetic baseline for acceptance only";
    authoring.surface_states.push_back(surface_state);

    condition::AuthoredStateDelta collision;
    collision.id = 6U;
    collision.target_record_id = surface_state.id;
    collision.target_asset_id = surface_state.asset_id;
    collision.target_kind = condition::StateTargetKind::surface_state;
    collision.property = condition::StateProperty::damage;
    collision.before.text_value = "intact";
    collision.after.text_value = "collision-damage";
    condition::AuthoredStateBranch damage_branch;
    damage_branch.id = 7U;
    damage_branch.name = "Low-speed collision damage";
    damage_branch.applicability = "after-low-speed-collision";
    damage_branch.deltas.push_back(collision);
    authoring.state_branches.push_back(std::move(damage_branch));

    require(static_cast<bool>(authoring.validate()),
        "synthetic pickup CONDITION source document did not validate");
    return authoring;
}

void run(const std::filesystem::path& output_root) {
    const auto started = std::chrono::steady_clock::now();
    std::filesystem::create_directories(output_root);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto fixture_root = output_root /
        ("p-" + std::to_string(stamp % 100'000'000));
    std::filesystem::create_directories(fixture_root);
    const auto project_path = fixture_root / "pickup-truck.carto";
    const auto source_v1_path = fixture_root / "pickup-source-v1.obj";
    const auto source_v2_path = fixture_root / "pickup-source-v2.obj";

    application::ApplicationSession session;
    ui::UiController controller(session);
    require(static_cast<bool>(controller.dispatch(application::NewProjectAction{
                "Synthetic Pickup Truck Authoring Fixture"})),
        "could not create pickup fixture project");
    require(static_cast<bool>(controller.dispatch(application::SaveProjectAction{project_path})),
        "pickup fixture project must be saved before source-backed import");

    auto authored_chassis = geometry::make_box({1.9, 1.0, 3.8});
    require(static_cast<bool>(authored_chassis), "pickup chassis blockout mesh is invalid");
    require(static_cast<bool>(io::export_obj(authored_chassis.value(), source_v1_path)),
        "pickup chassis OBJ fixture could not be exported");
    const auto source_v1 = read_text(source_v1_path);
    const auto decoded_v1 = io::import_obj(source_v1_path);
    require(static_cast<bool>(decoded_v1), "pickup OBJ fixture could not be imported");
    assets::BlobStore blobs(project_path.parent_path() / ".carto-assets" / "source-blobs");
    const auto blob_v1 = blobs.put(source_v1, "model/obj");
    require(static_cast<bool>(blob_v1), blob_v1
        ? "pickup source bytes were not content-addressed"
        : "pickup source bytes were not content-addressed: " + blob_v1.error().message);
    const auto chassis_digest = application::canonical_mesh_geometry_digest(decoded_v1.value().mesh);
    const auto initial_source = source_record_for(
        project_path, blobs, blob_v1.value(), chassis_digest, core::Revision{1U});
    require(static_cast<bool>(controller.dispatch(application::CreateMeshObjectAction{
                "Pickup chassis source", decoded_v1.value().mesh, initial_source})),
        "native source-backed pickup chassis import failed");
    auto snapshot = controller.snapshot();
    require(snapshot.source_assets.size() == 1U &&
            snapshot.source_assets.front().identity == initial_source.identity &&
            snapshot.source_assets.front().source_generation == core::Revision{1U},
        "pickup chassis lost its first source-registry generation");
    const auto chassis_object = snapshot.objects.back().object.id;
    const auto chassis_mesh = snapshot.objects.back().object.mesh_asset;
    require(chassis_mesh.has_value() && snapshot.objects.back().mesh_revision.has_value(),
        "source-backed pickup chassis has no mesh revision");

    // The changed comment is source metadata only: the real bounded OBJ importer
    // proves that the decoded geometry remains byte-for-byte equivalent.
    const std::string source_v2 = source_v1 + "\n# reviewed metadata-only refresh\n";
    {
        std::ofstream output(source_v2_path, std::ios::binary | std::ios::trunc);
        require(output.good(), "pickup source revision 2 could not be created");
        output.write(source_v2.data(), static_cast<std::streamsize>(source_v2.size()));
        require(output.good(), "pickup source revision 2 could not be written");
    }
    const auto decoded_v2 = io::import_obj(source_v2_path);
    require(static_cast<bool>(decoded_v2) &&
            application::canonical_mesh_geometry_digest(decoded_v2.value().mesh) == chassis_digest,
        "pickup metadata-only source refresh changed decoded geometry");
    const auto blob_v2 = blobs.put(source_v2, "model/obj");
    require(static_cast<bool>(blob_v2) && blob_v2.value().digest != blob_v1.value().digest,
        "pickup source revision did not produce a distinct content-addressed blob");
    auto refreshed_source = source_record_for(
        project_path, blobs, blob_v2.value(), chassis_digest, core::Revision{2U});
    refreshed_source.artifact_identity = snapshot.source_assets.front().artifact_identity;
    const auto before_plan = controller.snapshot();
    const auto reimport_plan = session.preview_source_reimport({refreshed_source});
    require(static_cast<bool>(reimport_plan) && reimport_plan.value().entries.size() == 1U,
        "pickup reimport plan did not contain exactly one source");
    require(reimport_plan.value().entries.front().change == assets::SourceReimportChange::metadata_only,
        "pickup reimport classification was " + std::to_string(static_cast<unsigned int>(
            reimport_plan.value().entries.front().change)) + " instead of metadata-only");
    require(controller.snapshot().project_revision == before_plan.project_revision &&
            controller.snapshot().operations.size() == before_plan.operations.size(),
        "pickup reimport preview mutated the project or journal");
    const auto reimport_receipt = controller.dispatch(
        application::ApplySourceReimportPlanAction{reimport_plan.value()});
    require(static_cast<bool>(reimport_receipt) && reimport_receipt.value().document_changed,
        "human-reviewed pickup source reimport failed");
    snapshot = controller.snapshot();
    require(snapshot.project_revision == before_plan.project_revision.next() &&
            snapshot.source_assets.front().source_generation == core::Revision{2U} &&
            snapshot.operations.size() == before_plan.operations.size() + 1U &&
            snapshot.operations.back().action == reimport_receipt.value().action &&
            snapshot.operations.back().revision_before == before_plan.project_revision &&
            snapshot.operations.back().revision_after == snapshot.project_revision &&
            snapshot.operations.back().document_changed,
        "pickup source reimport did not create one revisioned operation receipt");
    const auto stale_plan_snapshot = snapshot;
    const auto stale_plan = controller.dispatch(
        application::ApplySourceReimportPlanAction{reimport_plan.value()});
    require(!stale_plan && stale_plan.error().code == core::ErrorCode::stale_data &&
            controller.snapshot().project_revision == stale_plan_snapshot.project_revision &&
            controller.snapshot().operations.size() == stale_plan_snapshot.operations.size(),
        "pickup accepted a stale reviewed reimport plan or mutated on rejection");
    require(static_cast<bool>(controller.dispatch(application::UndoAction{})) &&
            controller.snapshot().source_assets.front().source_generation == core::Revision{3U} &&
            controller.snapshot().source_assets.front().source_digest == blob_v1.value().digest,
        "pickup undo failed to restore the prior source while preserving monotonic lineage");
    require(static_cast<bool>(controller.dispatch(application::RedoAction{})) &&
            controller.snapshot().source_assets.front().source_generation == core::Revision{4U} &&
            controller.snapshot().source_assets.front().source_digest == blob_v2.value().digest,
        "pickup redo failed to restore the reviewed source while preserving monotonic lineage");

    auto paint_mesh = box_with_material_region({1.7, 1.05, 1.55}, 1U);
    const Part cab = add_part(controller, "Pickup cab blockout", std::move(paint_mesh),
        {0.0, 0.92, 0.45});
    const Part bed = add_part(controller, "Pickup payload bed blockout",
        box_with_material_region({1.75, 0.55, 1.65}, 1U), {0.0, 0.62, -1.22});
    std::vector<Part> wheels;
    for (const auto& [name, position] : std::array<std::pair<std::string_view, core::Vec3d>, 4>{
            std::pair{"front-left", core::Vec3d{-0.94, 0.36, 1.15}},
            std::pair{"front-right", core::Vec3d{0.94, 0.36, 1.15}},
            std::pair{"rear-left", core::Vec3d{-0.94, 0.36, -1.18}},
            std::pair{"rear-right", core::Vec3d{0.94, 0.36, -1.18}}}) {
        wheels.push_back(add_part(controller,
            "Pickup " + std::string(name) + " wheel blockout",
            box_with_material_region({0.34, 0.72, 0.72}, 2U), position));
    }
    const Part tailgate = add_part(controller, "Pickup tailgate blockout",
        box_with_material_region({1.65, 0.72, 0.10}, 1U), {0.0, 0.86, -2.0});
    require(cab.object && bed.object && wheels.size() == 4U,
        "pickup mechanical blockout is missing a cab, bed, or wheel");

    auto authoring = pickup_authoring(
        refreshed_source, digest("synthetic-pickup-material-source-v1"));
    require(static_cast<bool>(controller.dispatch(application::ImportConditionAuthoringAction{
                authoring.serialize()})),
        "pickup CONDITION identity, variants, and state source did not import");
    snapshot = controller.snapshot();
    const auto binding = controller.bind_condition_asset_to_object(
        authoring.assets.front().id, chassis_object,
        snapshot.project_revision, snapshot.condition.authored_revision);
    require(binding && binding.value().document_changed,
        "pickup CONDITION assembly did not bind to its source chassis object");

    render::StandardMaterial paint;
    paint.base_color_factor = {0.13, 0.22, 0.27, 1.0};
    paint.metallic = 0.48;
    paint.roughness = 0.39;
    require(static_cast<bool>(controller.dispatch(application::CreateMaterialAction{
                controller.snapshot().project_revision, "Pickup blue-gray paint", paint})),
        "pickup paint source material could not be authored");
    const auto paint_id = controller.snapshot().materials.back().id;
    render::StandardMaterial rubber;
    rubber.base_color_factor = {0.025, 0.028, 0.03, 1.0};
    rubber.metallic = 0.0;
    rubber.roughness = 0.91;
    require(static_cast<bool>(controller.dispatch(application::CreateMaterialAction{
                controller.snapshot().project_revision, "Pickup tire rubber", rubber})),
        "pickup tire material could not be authored");
    const auto rubber_id = controller.snapshot().materials.back().id;
    require(static_cast<bool>(controller.dispatch(application::SetMaterialRegionAssignmentAction{
                controller.snapshot().project_revision,
                project::MaterialRegionKey{cab.mesh_asset, 1U}, paint_id, std::nullopt})) &&
            static_cast<bool>(controller.dispatch(application::SetMaterialRegionAssignmentAction{
                controller.snapshot().project_revision,
                project::MaterialRegionKey{wheels.front().mesh_asset, 2U}, rubber_id,
                std::nullopt})),
        "pickup paint and tire material-region assignments failed");

    const Part camera_marker = add_part(controller, "Pickup review camera marker",
        marker_mesh(), {4.0, 2.0, 5.0});
    const Part left_light = add_part(controller, "Pickup left headlight marker",
        marker_mesh(), {-0.55, 0.95, 1.93});
    const Part right_light = add_part(controller, "Pickup right headlight marker",
        marker_mesh(), {0.55, 0.95, 1.93});
    project::PresentationCatalog presentation;
    project::AuthoredCamera camera;
    camera.object = camera_marker.object;
    camera.identity = "condition.fixture.pickup.review-camera";
    camera.bookmark_identity = "pickup.exterior.review";
    camera.projection = project::CameraProjection::perspective;
    camera.review_purpose = project::CameraReviewPurpose::screenshot_evidence;
    camera.active_for_review = true;
    camera.far_plane = 500.0;
    camera.revision = core::Revision{1U};
    require(static_cast<bool>(presentation.insert_camera(camera)),
        "pickup review camera intent was invalid");
    for (const auto& [part, identity] : std::array<std::pair<Part, std::string_view>, 2>{
            std::pair{left_light, std::string_view{"pickup.headlight.left"}},
            std::pair{right_light, std::string_view{"pickup.headlight.right"}}}) {
        project::AuthoredLight light;
        light.object = part.object;
        light.identity = identity;
        light.semantic_class = "vehicle.headlight";
        light.fixture_owner = "pickup.truck";
        light.power_owner = "pickup.electrical";
        light.switch_owner = "pickup.headlight-switch";
        light.kind = project::AuthoredLightKind::point;
        light.revision = core::Revision{1U};
        light.linear_color = {1.0, 0.82, 0.62};
        light.intensity = 18.0;
        light.range = 34.0;
        light.normal_state = project::LightNormalState::switchable;
        require(static_cast<bool>(presentation.insert_light(light)),
            "pickup headlight source intent was invalid");
    }
    snapshot = controller.snapshot();
    const auto presentation_receipt = controller.dispatch(
        application::SetPresentationCatalogAction{
            snapshot.project_generation, snapshot.project_revision,
            std::move(presentation)});
    require(static_cast<bool>(presentation_receipt),
        presentation_receipt
            ? "pickup review camera and lighting intent did not commit"
            : "pickup presentation intent rejected: " + presentation_receipt.error().message);

    kinematic::KinematicGraph graph;
    kinematic::KinematicNode frame;
    frame.id = kinematic::JointId{1U};
    frame.kind = kinematic::JointKind::fixed;
    frame.semantic_type = "vehicle.frame";
    require(static_cast<bool>(graph.insert_node(frame)),
        "pickup kinematic root could not be authored");
    kinematic::JointLimitSet limits;
    condition::KinematicAssemblySource assembly;
    assembly.asset_id = authoring.assets.front().id;
    const auto add_hinge = [&](std::uint64_t joint_id, std::string semantic,
                               core::Vec3d pivot, const Part& part,
                               double minimum, double maximum) {
        kinematic::KinematicNode node;
        node.id = kinematic::JointId{joint_id};
        node.parent = kinematic::JointId{1U};
        node.kind = kinematic::JointKind::hinge;
        node.rest.translation = pivot;
        node.semantic_type = std::move(semantic);
        require(static_cast<bool>(graph.insert_node(node)),
            "pickup movable source joint could not be authored");
        kinematic::JointLimit limit;
        limit.joint = node.id;
        limit.angular_enabled = true;
        limit.angular_min = {minimum, 0.0, 0.0};
        limit.angular_max = {maximum, 0.0, 0.0};
        require(static_cast<bool>(limits.insert(limit)),
            "pickup source joint travel limits were rejected");
        assembly.part_bindings.push_back(condition::KinematicAssemblySource::PartBinding{
            joint_id, part.object.value, part.mesh_asset, part.mesh_revision});
    };
    for (std::size_t index = 0U; index < wheels.size(); ++index) {
        add_hinge(static_cast<std::uint64_t>(index) + 2U, "vehicle.wheel.hinge",
            wheels[index].position, wheels[index], -0.8, 0.8);
    }
    add_hinge(6U, "vehicle.tailgate.hinge", tailgate.position, tailgate, 0.0, 1.2);
    assembly.graph_bytes = graph.serialize();
    assembly.joint_limit_bytes = limits.serialize();
    snapshot = controller.snapshot();
    auto stale_assembly = assembly;
    stale_assembly.part_bindings.front().mesh_revision =
        stale_assembly.part_bindings.front().mesh_revision.next();
    require(!controller.dispatch(application::AddConditionKinematicAssemblyAction{
                snapshot.project_revision, snapshot.condition.authored_revision,
                stale_assembly}) &&
            controller.snapshot().project_revision == snapshot.project_revision,
        "pickup articulation accepted a stale wheel mesh binding");
    require(static_cast<bool>(controller.dispatch(application::AddConditionKinematicAssemblyAction{
                snapshot.project_revision, snapshot.condition.authored_revision, assembly})),
        "pickup wheels and tailgate did not enter the revision-bound kinematic source");
    snapshot = controller.snapshot();
    require(snapshot.condition.kinematic_assemblies.size() == 1U &&
            snapshot.condition.kinematic_assemblies.front().part_binding_count == 5U &&
            snapshot.condition.kinematic_assemblies.front().part_bindings_current,
        "pickup articulation snapshot lost one or more current movable part bindings");
    const auto assembly_id = snapshot.condition.kinematic_assemblies.front().id;
    const auto operations_before_preview = snapshot.operations.size();
    const auto wheel_pose = controller.preview_condition_kinematic_hinge(
        snapshot, assembly_id, 2U, 1.0);
    const auto tailgate_pose = controller.preview_condition_kinematic_hinge(
        snapshot, assembly_id, 6U, 1.0);
    require(wheel_pose && tailgate_pose &&
            wheel_pose.value().part_poses.size() == 1U &&
            tailgate_pose.value().part_poses.size() == 1U &&
            wheel_pose.value().part_poses.front().scene_object_id == wheels.front().object.value &&
            tailgate_pose.value().part_poses.front().scene_object_id == tailgate.object.value &&
            std::abs(wheel_pose.value().part_poses.front().world_transform.rotation.x) > 1.0e-6 &&
            controller.snapshot().project_revision == snapshot.project_revision &&
            controller.snapshot().operations.size() == operations_before_preview,
        "pickup wheel/tailgate preview was not bound, visible in pose output, or read-only");

    const auto before_invalidation = controller.snapshot();
    const auto source_v2_digest = refreshed_source.source_digest;
    require(static_cast<bool>(controller.dispatch(application::InvalidateConditionSourceAction{
                1U, "2", source_v2_digest})),
        "pickup source invalidation was not routed through the application command boundary");
    const auto stale_authoring = session.condition_authoring();
    require(stale_authoring && stale_authoring.value().has_stale_sources() &&
            !controller.build_condition_package() &&
            controller.snapshot().project_revision == before_invalidation.project_revision.next(),
        "pickup stale evidence remained publishable or changed outside one transaction");

    auto reconciled = stale_authoring.value();
    reconciled.project_revision = reconciled.project_revision.next();
    auto evidence = std::find_if(reconciled.sources.begin(), reconciled.sources.end(),
        [](const auto& item) { return item.id == 1U; });
    require(evidence != reconciled.sources.end(), "pickup source evidence disappeared");
    evidence->revision = "2";
    evidence->digest = source_v2_digest;
    evidence->reviewed = true;
    evidence->current = true;
    evidence->diagnostics.clear();
    auto asset = std::find_if(reconciled.assets.begin(), reconciled.assets.end(),
        [](const auto& item) { return item.id == 2U; });
    require(asset != reconciled.assets.end(), "pickup source asset disappeared");
    asset->source_revision = "2";
    asset->source_digest = source_v2_digest;
    const auto before_reconcile = controller.snapshot();
    require(static_cast<bool>(controller.dispatch(application::ImportConditionAuthoringAction{
                reconciled.serialize()})) &&
            controller.snapshot().project_revision == before_reconcile.project_revision.next() &&
            !session.condition_authoring().value().has_stale_sources() &&
            static_cast<bool>(controller.build_condition_package()),
        "human reconciliation did not restore exact current pickup evidence and publication readiness");

    const auto final_snapshot = controller.snapshot();
    require(final_snapshot.condition.variants.size() == 2U &&
            final_snapshot.condition.surface_state_count == 1U &&
            final_snapshot.materials.size() == 2U &&
            final_snapshot.material_assignments.size() == 2U &&
            final_snapshot.presentation_catalog.cameras().size() == 1U &&
            final_snapshot.presentation_catalog.lights().size() == 2U,
        "pickup fixture did not retain its variants, damage baseline, materials, camera, and lights");
    require(static_cast<bool>(controller.dispatch(application::SaveProjectAction{std::nullopt})),
        "pickup project could not be saved after authoring completion");

    const auto reopened_project = project::ProjectDocument::load(project_path);
    require(static_cast<bool>(reopened_project), "pickup .carto project failed to reopen");
    const auto& registry = reopened_project.value().source_asset_registry().records();
    require(registry.size() == 1U && registry.at(initial_source.identity).source_generation ==
            core::Revision{4U} && registry.at(initial_source.identity).source_digest == source_v2_digest &&
            !std::filesystem::path(registry.at(initial_source.identity).relative_source_path).is_absolute(),
        "pickup .carto lost source generation, digest, or project-relative portability");
    require(reopened_project.value().presentation_catalog().cameras().size() == 1U &&
            reopened_project.value().presentation_catalog().lights().size() == 2U,
        "pickup .carto lost authored presentation intent");
    const auto serialized_project = reopened_project.value().serialize();
    for (const auto forbidden : {"DONOR_REPOSITORY", "DONOR_COMMIT", "runtime_handle",
             "provider_handle", "worker_endpoint", "telemetry"}) {
        require(serialized_project.find(forbidden) == std::string::npos,
            "pickup .carto serialized private donor identity or runtime-only state");
    }
    require(serialized_project.find(project_path.parent_path().string()) == std::string::npos,
        "pickup .carto serialized the workstation project path");

    application::ApplicationSession reopened_session;
    ui::UiController reopened_controller(reopened_session);
    require(static_cast<bool>(reopened_controller.dispatch(
                application::OpenProjectAction{project_path})),
        "native application could not reopen the completed pickup fixture");
    const auto reopened_snapshot = reopened_controller.snapshot();
    const auto reopened_authoring = reopened_session.condition_authoring();
    require(reopened_snapshot.project_revision == final_snapshot.project_revision &&
            reopened_snapshot.objects.size() == final_snapshot.objects.size() &&
            reopened_snapshot.source_assets.front().source_generation == core::Revision{4U} &&
            reopened_snapshot.material_assignments.size() == 2U &&
            reopened_authoring && reopened_authoring.value().variants.size() == 2U &&
            reopened_authoring.value().surface_states.size() == 1U &&
            reopened_authoring.value().state_branches.size() == 1U &&
            reopened_authoring.value().kinematic_assemblies.size() == 1U &&
            reopened_snapshot.condition.kinematic_assemblies.front().part_bindings_current,
        "native pickup reopen did not preserve source, state, variant, and articulation identity");

    const auto package = condition::Package::publish(
        reopened_authoring.value(), reopened_snapshot.project_revision,
        reopened_authoring.value().project_namespace);
    require(static_cast<bool>(package),
        "reconciled Pickup source did not produce a CONDITION publication package");
    const auto admission = condition::admit_for_consumer(package.value());
    require(admission && admission.value().status == condition::PackageAdmissionStatus::admit &&
            admission.value().admitted_authoring_digest == package.value().authoring_digest,
        "reopened Pickup package failed consumer admission or changed authoring identity");
    const auto serialized_package = package.value().serialize();
    for (const auto forbidden : {"DONOR_REPOSITORY", "DONOR_COMMIT", "runtime_handle",
             "provider_handle", "worker_endpoint", "telemetry"}) {
        require(serialized_package.find(forbidden) == std::string::npos,
            "Pickup package serialized private donor identity or runtime-only state");
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "pickup_fixture_project=" << project_path.string()
              << " source_generation=4 source_reimport=metadata-only"
              << " components=" << reopened_snapshot.objects.size()
              << " kinematic_parts=5 materials=2 authored_lights=2"
              << " stale_source_rejected=true save_reopen=true consumer_admitted=true"
              << " private_runtime=false"
              << " geometry_quality=synthetic-blockout"
              << " fixture_wall_ms=" << elapsed_ms
              << " render_performance=not-measured visual_capture=not-performed\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error(
                "usage: cartographer_condition_pickup_fixture_tests <output-root>");
        }
        run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
