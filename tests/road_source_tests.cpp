#include <carto/journal/journal.hpp>
#include <carto/geometry/road_surface.hpp>
#include <carto/project/project.hpp>
#include <carto/project/road_source.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-road-source-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

geometry::CurvePath make_centerline(core::Revision revision) {
    geometry::CurvePath path;
    path.source_identity = "road.pine-ridge.centerline";
    path.source_revision = revision;
    path.segments.push_back({
        {0.0, 0.0, 0.0}, {1.0, 2.0, 0.0},
        {2.0, 2.0, 2.0}, {3.0, 0.0, 2.0}});
    return path;
}

project::RoadSource make_road(
    core::Revision source_revision = core::Revision{1U},
    core::Revision centerline_revision = core::Revision{1U}) {
    project::RoadSource road;
    road.source_identity = "road.pine-ridge.approach";
    road.source_revision = source_revision;
    road.centerline_identity = "road.pine-ridge.centerline";
    road.centerline_revision = centerline_revision;
    road.width_meters = 6.0;
    road.shoulder_meters = 1.0;
    road.maximum_grade_ratio = 0.12;
    road.minimum_curve_radius_meters = 24.0;
    road.surface_class = project::RoadSurfaceClass::asphalt;
    road.function_class = project::RoadFunctionClass::local;
    road.access_state = project::RoadAccessState::open;
    road.drainage_intent = project::RoadDrainageIntent::roadside_ditch;
    return road;
}

geometry::CurvePath straight_centerline() {
    geometry::CurvePath path;
    path.source_identity = "road.test.straight";
    path.source_revision = core::Revision{1U};
    path.segments.push_back({
        {0.0, 0.0, 0.0}, {10.0 / 3.0, 0.0, 0.0},
        {20.0 / 3.0, 0.0, 0.0}, {10.0, 0.0, 0.0}});
    return path;
}

void derived_road_surface_is_bounded_and_fails_closed() {
    const auto centerline = straight_centerline();
    const auto preview = geometry::build_road_surface(
        centerline, 6.0, 1.0, 0.12, 20.0, 8U);
    require(preview && preview.value().validate(),
        "valid source centerline produces a validated disposable road ribbon");
    require(preview.value().stations.size() == 9U &&
            preview.value().road_triangle_count() == 16U &&
            preview.value().shoulder_triangle_count() == 32U,
        "ribbon station and triangle counts are deterministic and bounded");
    require(preview.value().stations.front().road_left.z == 3.0 &&
            preview.value().stations.front().road_right.z == -3.0 &&
            preview.value().stations.front().shoulder_left.z == 4.0 &&
            preview.value().stations.front().shoulder_right.z == -4.0,
        "road and shoulder widths are applied symmetrically around the source");
    const auto repeated = geometry::build_road_surface(
        centerline, 6.0, 1.0, 0.12, 20.0, 8U);
    require(repeated && repeated.value().stations.size() == preview.value().stations.size() &&
            repeated.value().stations.back().center.x == preview.value().stations.back().center.x,
        "identical source and sampling inputs yield a deterministic preview");

    auto steep = centerline;
    steep.segments.front().p1.y = 2.0;
    steep.segments.front().p2.y = 8.0;
    steep.segments.front().p3.y = 10.0;
    require(!geometry::build_road_surface(steep, 6.0, 1.0, 0.1, 20.0, 8U),
        "sampled grade above the road design limit is rejected");

    auto tight_curve = centerline;
    tight_curve.segments.front() = {
        {0.0, 0.0, 0.0}, {0.0, 0.0, 4.0},
        {4.0, 0.0, 8.0}, {8.0, 0.0, 8.0}};
    require(!geometry::build_road_surface(tight_curve, 6.0, 1.0, 0.12, 20.0, 32U),
        "sampled curvature tighter than the minimum radius is rejected");

    auto reversing = centerline;
    reversing.segments = {
        {{0.0, 0.0, 0.0}, {3.0, 0.0, 0.0},
         {7.0, 0.0, 0.0}, {10.0, 0.0, 0.0}},
        {{10.0, 0.0, 0.0}, {7.0, 0.0, 0.0},
         {3.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
    require(!geometry::build_road_surface(reversing, 6.0, 1.0, 0.12, 20.0, 8U),
        "a reversing centerline that would fold the ribbon is rejected");
    require(!geometry::build_road_surface(centerline, 100.0, 25.0, 0.12, 20.0, 8U),
        "a road envelope wider than its minimum radius is rejected");
    require(!geometry::build_road_surface(centerline, 6.0, 1.0, 0.12, 20.0, 0U),
        "zero-density sampling is rejected before geometry allocation");
    require(!geometry::build_road_surface(centerline, 6.0, 1.0, 0.12, 20.0, 4097U),
        "oversized sampling requests are rejected");

    auto corrupted = preview.value();
    corrupted.stations.back().road_left.x = std::numeric_limits<double>::quiet_NaN();
    require(!corrupted.validate(),
        "non-finite derived station data cannot pass product validation");
}

void road_surface_builds_editable_material_and_world_scale_uv_regions() {
    const auto centerline = straight_centerline();
    const auto preview = geometry::build_road_surface(
        centerline, 6.0, 1.0, 0.12, 20.0, 8U);
    require(static_cast<bool>(preview),
        "valid road preview is available to the editable-mesh builder");
    const auto mesh = geometry::build_road_surface_editable_mesh(preview.value());
    require(mesh && mesh.value().validate(),
        "current road ribbon materializes as a valid editable mesh");
    require(mesh.value().vertex_count() == preview.value().stations.size() * 4U &&
            mesh.value().face_count() == (preview.value().stations.size() - 1U) * 3U,
        "road and shoulder strips produce the expected bounded station topology");

    const auto* slots = mesh.value().attributes().find(
        geometry::kRoadSurfaceMaterialSlotLayerIdentity);
    const auto* uvs = mesh.value().attributes().find(
        geometry::kRoadSurfaceUvLayerIdentity);
    const auto* uv_set = mesh.value().uv_sets().find(geometry::kRoadSurfaceUvSetId);
    require(slots != nullptr && slots->descriptor.domain == attributes::AttributeDomain::face &&
            slots->descriptor.type == attributes::AttributeType::uint32 &&
            slots->values.size() == mesh.value().face_count(),
        "road strips retain a face-domain material-region attribute");
    require(uvs != nullptr && uvs->descriptor.domain == attributes::AttributeDomain::corner &&
            uvs->descriptor.type == attributes::AttributeType::vec2 &&
            uvs->values.size() == mesh.value().topology().value().corners.size() &&
            uv_set != nullptr && uv_set->active_for_editing && uv_set->active_for_render,
        "road UVs are explicit corner data and active for editing and rendering");
    const auto road_face_count = static_cast<std::size_t>(std::count_if(
        slots->values.begin(), slots->values.end(), [](const auto& value) {
            return std::get<std::uint32_t>(value) ==
                geometry::kRoadSurfaceRoadMaterialSlot;
        }));
    const auto shoulder_face_count = static_cast<std::size_t>(std::count_if(
        slots->values.begin(), slots->values.end(), [](const auto& value) {
            return std::get<std::uint32_t>(value) ==
                geometry::kRoadSurfaceShoulderMaterialSlot;
        }));
    require(road_face_count == preview.value().stations.size() - 1U &&
            shoulder_face_count == (preview.value().stations.size() - 1U) * 2U,
        "road and shoulder faces remain independently assignable material regions");

    double maximum_longitudinal_uv = 0.0;
    double maximum_lateral_uv = 0.0;
    for (const auto& value : uvs->values) {
        const auto uv = std::get<core::Vec2d>(value);
        require(uv.finite() && uv.x >= 0.0 && uv.y >= 0.0,
            "road UVs are finite, nonnegative world-scale coordinates");
        maximum_longitudinal_uv = std::max(maximum_longitudinal_uv, uv.x);
        maximum_lateral_uv = std::max(maximum_lateral_uv, uv.y);
    }
    require(std::abs(maximum_longitudinal_uv - 10.0) < 1.0e-8 &&
            std::abs(maximum_lateral_uv - 8.0) < 1.0e-8,
        "road UV extents follow traveled distance and road-plus-shoulder width in meters");

    const auto no_shoulder_preview = geometry::build_road_surface(
        centerline, 6.0, 0.0, 0.12, 20.0, 8U);
    require(static_cast<bool>(no_shoulder_preview),
        "road preview without shoulders is valid");
    const auto no_shoulder_mesh = geometry::build_road_surface_editable_mesh(
        no_shoulder_preview.value());
    require(no_shoulder_mesh && no_shoulder_mesh.value().vertex_count() ==
                no_shoulder_preview.value().stations.size() * 2U &&
            no_shoulder_mesh.value().face_count() ==
                no_shoulder_preview.value().stations.size() - 1U,
        "zero-width shoulders do not create duplicate or degenerate geometry");

    auto corrupted = preview.value();
    corrupted.stations.front().road_left.x = std::numeric_limits<double>::quiet_NaN();
    require(!geometry::build_road_surface_editable_mesh(corrupted),
        "invalid preview data cannot be partially committed as editable geometry");
}

void road_source_contract_is_bounded_and_canonical() {
    const auto road = make_road();
    require(static_cast<bool>(road.validate()), "valid road source is admitted");
    const std::string encoded = road.serialize();
    const auto reopened = project::RoadSource::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "road source has deterministic canonical serialization");
    require(road.source_digest() == reopened.value().source_digest(),
        "road source digest binds canonical source fields");

    const auto centerline = make_centerline(core::Revision{1U});
    const auto bounds = road.coverage_bounds(centerline);
    require(bounds && bounds.value().min_x == -4.0 && bounds.value().max_x == 7.0 &&
            bounds.value().min_y == 0.0 && bounds.value().max_y == 2.0 &&
            bounds.value().min_z == -4.0 && bounds.value().max_z == 6.0,
        "conservative road coverage uses linked control bounds and shoulder width");
    auto stale_centerline = centerline;
    stale_centerline.source_revision = core::Revision{2U};
    require(!road.coverage_bounds(stale_centerline),
        "coverage refuses a stale centerline revision");

    auto invalid = road;
    invalid.source_identity = "../private";
    require(!invalid.validate(), "path-shaped road identities are rejected");
    invalid = road;
    invalid.width_meters = std::numeric_limits<double>::quiet_NaN();
    require(!invalid.validate(), "non-finite road dimensions are rejected");
    invalid = road;
    invalid.maximum_grade_ratio = 1.01;
    require(!invalid.validate(), "out-of-range grade policy is rejected");
    invalid = road;
    invalid.function_class = static_cast<project::RoadFunctionClass>(255U);
    require(!invalid.validate(), "unknown road classifications are rejected");

    require(!project::RoadSource::deserialize(encoded + "unexpected\n"),
        "trailing serialized data is rejected");
    std::string future = encoded;
    const auto schema = future.find("CARTOGRAPHER_ROAD_SOURCE 1");
    require(schema != std::string::npos, "road source schema marker exists");
    future.replace(schema, std::string("CARTOGRAPHER_ROAD_SOURCE 1").size(),
        "CARTOGRAPHER_ROAD_SOURCE 99");
    require(!project::RoadSource::deserialize(future),
        "unsupported road source schema is rejected");
}

void project_transactions_bind_road_to_exact_curve_revision() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "road-source-events.log");

    auto missing_centerline = project::ProjectTransaction::begin(
        document, journal, "road-source-tests");
    require(missing_centerline && !missing_centerline.value().set_road_source(make_road()),
        "road source cannot bind to a missing centerline");
    require(static_cast<bool>(missing_centerline.value().rollback()),
        "missing-centerline mutation rolls back without publication");

    auto create = project::ProjectTransaction::begin(document, journal, "road-source-tests");
    require(create && create.value().set_curve_path(make_centerline(core::Revision{1U})) &&
            create.value().set_road_source(make_road()),
        "centerline and road source are staged in a governed transaction");
    const auto created = create.value().commit("road.source.create");
    require(created && document.road_sources().size() == 1U &&
            document.road_sources().at("road.pine-ridge.approach").centerline_revision ==
                core::Revision{1U},
        "road source commits with an exact stable centerline binding");

    auto stale_curve = make_centerline(core::Revision{2U});
    auto invalid_update = project::ProjectTransaction::begin(
        document, journal, "road-source-tests");
    require(invalid_update && invalid_update.value().set_curve_path(stale_curve),
        "a centerline edit can be staged for atomic road review");
    const auto rejected = invalid_update.value().commit("road.centerline.stale-road");
    require(!rejected, "centerline edit cannot publish while road binding is stale");
    require(static_cast<bool>(invalid_update.value().rollback()),
        "failed stale-binding transaction can be discarded");
    require(document.curve_paths().at("road.pine-ridge.centerline").source_revision ==
                core::Revision{1U},
        "failed stale-binding commit preserves the authoritative project");

    stale_curve.segments.front().p1.y = 1.5;
    stale_curve.segments.front().p2.y = 1.5;
    auto update = project::ProjectTransaction::begin(document, journal, "road-source-tests");
    auto updated_road = make_road(core::Revision{2U}, core::Revision{2U});
    updated_road.access_state = project::RoadAccessState::restricted;
    updated_road.drainage_intent = project::RoadDrainageIntent::culvert;
    require(update && update.value().set_curve_path(stale_curve) &&
            update.value().set_road_source(updated_road),
        "road geometry, access state, and drainage intent can update together");
    const auto updated = update.value().commit("road.source.update");
    require(updated && document.road_sources().at("road.pine-ridge.approach").source_revision ==
                core::Revision{2U},
        "road source revision advances atomically with centerline revision");

    auto stale_road = make_road(core::Revision{2U}, core::Revision{1U});
    auto stale_update = project::ProjectTransaction::begin(
        document, journal, "road-source-tests");
    require(stale_update && !stale_update.value().set_road_source(stale_road),
        "road mutation cannot bind to a stale centerline revision");
    require(static_cast<bool>(stale_update.value().rollback()),
        "rejected road update rolls back cleanly");

    const auto events = journal.read_all();
    require(events && events.value().size() == 2U &&
            events.value()[0U].event_type == "road.source.create" &&
            events.value()[1U].event_type == "road.source.update",
        "only valid road source transitions emit journal events");

    const std::string encoded = document.serialize();
    const std::string current_project_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion) + "\n";
    require(encoded.starts_with(current_project_header),
        "road and spatial-field persistence uses the current project schema");
    const auto reopened = project::ProjectDocument::deserialize(encoded);
    require(reopened && reopened.value().road_sources().size() == 1U &&
            reopened.value().road_sources().at("road.pine-ridge.approach").serialize() ==
                updated_road.serialize(),
        "project round-trip preserves road semantics and source lineage");
    const auto project_path = directory.path() / "pine-ridge-road.carto";
    const auto saved = document.save_atomic(project_path);
    require(static_cast<bool>(saved), saved
        ? "project atomically saves its road source"
        : "project atomically saves its road source: " + saved.error().message);
    const auto loaded = project::ProjectDocument::load(project_path);
    require(loaded && loaded.value().road_sources().at("road.pine-ridge.approach").serialize() ==
            updated_road.serialize(),
        "road source survives project close and reopen");

    std::string legacy_v16 = encoded;
    const auto table = legacy_v16.find("ROAD_SOURCES ");
    const auto end = legacy_v16.rfind("END\n");
    require(table != std::string::npos && end != std::string::npos,
        "road table can be isolated for v16 migration check");
    legacy_v16.replace(table, end + 4U - table, "END\n");
    const auto mesh_patches = legacy_v16.find("MESH_PATCHES ");
    require(mesh_patches != std::string::npos,
        "current project contains the schema-v29 mesh-patch table");
    const auto mesh_patches_end = legacy_v16.find('\n', mesh_patches);
    require(mesh_patches_end != std::string::npos &&
            legacy_v16.substr(mesh_patches, mesh_patches_end - mesh_patches) ==
                "MESH_PATCHES 0",
        "legacy road-source fixture has no mesh patches to migrate");
    legacy_v16.erase(mesh_patches, mesh_patches_end + 1U - mesh_patches);
    legacy_v16.replace(0U, std::string("CARTOGRAPHER_PROJECT 17").size(),
        "CARTOGRAPHER_PROJECT 16");
    const auto migrated = project::ProjectDocument::deserialize(legacy_v16);
    if (!migrated) std::cerr << "v16 migration: " << migrated.error().message << '\n';
    require(migrated && migrated.value().road_sources().empty() &&
            migrated.value().curve_paths().size() == 1U,
        "schema-v16 project migrates with no invented road sources");

    std::string missing_table = encoded;
    const auto spatial_table = missing_table.find("SPATIAL_FIELDS ");
    require(spatial_table != std::string::npos,
        "current project contains the spatial table after road records");
    missing_table.erase(table, spatial_table - table);
    require(!project::ProjectDocument::deserialize(missing_table),
        "schema-v20 refuses a missing mandatory road-source table");

    std::string legacy_v19 = encoded;
    const auto v19_mesh_patches = legacy_v19.find("MESH_PATCHES ");
    const auto v19_mesh_patches_end = legacy_v19.find('\n', v19_mesh_patches);
    require(v19_mesh_patches != std::string::npos &&
            v19_mesh_patches_end != std::string::npos &&
            legacy_v19.substr(v19_mesh_patches,
                v19_mesh_patches_end - v19_mesh_patches) == "MESH_PATCHES 0",
        "legacy v19 road-source fixture has no schema-v29 mesh patches");
    legacy_v19.erase(v19_mesh_patches,
        v19_mesh_patches_end + 1U - v19_mesh_patches);
    const auto sweep_table = legacy_v19.find("SWEEP_SOURCES ");
    const auto legacy_spatial_table = legacy_v19.find("SPATIAL_FIELDS ", sweep_table);
    require(sweep_table != std::string::npos && legacy_spatial_table != std::string::npos,
        "v19 migration fixture can isolate the later sweep-source table");
    legacy_v19.erase(sweep_table, legacy_spatial_table - sweep_table);
    const auto curve_lineage = legacy_v19.find("CURVE_PATH_LINEAGE ");
    const auto terminal = legacy_v19.rfind("END\n");
    require(curve_lineage != std::string::npos && terminal != std::string::npos,
        "v20 source-lineage tables can be isolated for v19 migration");
    legacy_v19.replace(curve_lineage, terminal + 4U - curve_lineage, "END\n");
    legacy_v19.replace(0U, std::string("CARTOGRAPHER_PROJECT 20").size(),
        "CARTOGRAPHER_PROJECT 19");
    const auto migrated_v19 = project::ProjectDocument::deserialize(legacy_v19);
    require(migrated_v19 && migrated_v19.value().curve_paths().size() == 1U &&
            migrated_v19.value().road_sources().size() == 1U,
        "v19 migration derives lineage only from its active curve and road records");

    auto missing_curve_lineage = encoded;
    const auto curve_lineage_start = missing_curve_lineage.find("CURVE_PATH_LINEAGE ");
    const auto current_terminal = missing_curve_lineage.rfind("END\n");
    require(curve_lineage_start != std::string::npos &&
            current_terminal != std::string::npos,
        "v20 curve lineage table is present");
    missing_curve_lineage.replace(curve_lineage_start,
        current_terminal + 4U - curve_lineage_start, "END\n");
    require(!project::ProjectDocument::deserialize(missing_curve_lineage),
        "v20 fails closed when curve lineage is omitted");

    auto missing_road_lineage = encoded;
    const auto road_lineage_start = missing_road_lineage.find("ROAD_SOURCE_LINEAGE ");
    const auto road_terminal = missing_road_lineage.rfind("END\n");
    require(road_lineage_start != std::string::npos && road_terminal != std::string::npos,
        "v20 road lineage table is present");
    missing_road_lineage.replace(road_lineage_start,
        road_terminal + 4U - road_lineage_start, "END\n");
    require(!project::ProjectDocument::deserialize(missing_road_lineage),
        "v20 fails closed when road lineage is omitted");

    auto conflicting_lineage = encoded;
    const auto active_curve_lineage = conflicting_lineage.find(
        "CURVE_PATH_LINEAGE_RECORD \"road.pine-ridge.centerline\" 2");
    require(active_curve_lineage != std::string::npos,
        "active curve lineage record is available for tamper testing");
    conflicting_lineage.replace(active_curve_lineage,
        std::string("CURVE_PATH_LINEAGE_RECORD \"road.pine-ridge.centerline\" 2").size(),
        "CURVE_PATH_LINEAGE_RECORD \"road.pine-ridge.centerline\" 1");
    require(!project::ProjectDocument::deserialize(conflicting_lineage),
        "active source revision cannot disagree with its retained lineage");
}

void curve_and_road_tombstones_prevent_source_revision_reuse() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "road-lineage-events.log");

    auto create = project::ProjectTransaction::begin(document, journal, "road-lineage-tests");
    require(create && create.value().set_curve_path(make_centerline(core::Revision{1U})) &&
            create.value().set_road_source(make_road()),
        "paired curve and road sources create in one transaction");
    require(static_cast<bool>(create.value().commit("road.pair.create")),
        "paired sources are journaled");

    auto remove = project::ProjectTransaction::begin(document, journal, "road-lineage-tests");
    require(remove && remove.value().erase_road_source(
                "road.pine-ridge.approach", core::Revision{1U}) &&
            remove.value().erase_curve_path(
                "road.pine-ridge.centerline", core::Revision{1U}),
        "dependent road must be erased before its centerline");
    require(static_cast<bool>(remove.value().commit("road.pair.remove")),
        "source removal is journaled");
    require(document.curve_paths().empty() && document.road_sources().empty(),
        "removal clears active sources but retains their lineages");

    const std::string deleted_snapshot = document.serialize();
    require(deleted_snapshot.find(
                "CURVE_PATH_LINEAGE_RECORD \"road.pine-ridge.centerline\" 1") !=
                std::string::npos &&
            deleted_snapshot.find(
                "ROAD_SOURCE_LINEAGE_RECORD \"road.pine-ridge.approach\" 1") !=
                std::string::npos,
        "serialized tombstones retain both latest source revisions");
    const auto reopened_deleted = project::ProjectDocument::deserialize(deleted_snapshot);
    require(reopened_deleted && reopened_deleted.value().curve_paths().empty() &&
            reopened_deleted.value().road_sources().empty(),
        "deleted-source lineages survive save/reopen");

    auto recreate = project::ProjectTransaction::begin(
        document, journal, "road-lineage-tests");
    auto reused_curve = make_centerline(core::Revision{1U});
    auto reused_road = make_road(core::Revision{1U}, core::Revision{1U});
    require(recreate && !recreate.value().set_curve_path(reused_curve),
        "deleted curve identity cannot be recreated at its stale revision");
    reused_curve.source_revision = core::Revision{2U};
    reused_road.source_revision = core::Revision{2U};
    reused_road.centerline_revision = core::Revision{2U};
    require(recreate.value().set_curve_path(reused_curve) &&
            recreate.value().set_road_source(reused_road),
        "both identities can be recreated only at their next source revisions");
    require(static_cast<bool>(recreate.value().commit("road.pair.recreate")),
        "recreated sources commit with retained lineage");
    require(document.curve_paths().at("road.pine-ridge.centerline").source_revision ==
                core::Revision{2U} &&
            document.road_sources().at("road.pine-ridge.approach").source_revision ==
                core::Revision{2U},
        "recreated source pair advances both independent lineages");
}

} // namespace

int main() {
    derived_road_surface_is_bounded_and_fails_closed();
    road_surface_builds_editable_material_and_world_scale_uv_regions();
    road_source_contract_is_bounded_and_canonical();
    project_transactions_bind_road_to_exact_curve_revision();
    curve_and_road_tombstones_prevent_source_revision_reuse();
    std::cout << "road source tests passed\n";
    return EXIT_SUCCESS;
}
