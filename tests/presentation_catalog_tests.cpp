#include <carto/journal/journal.hpp>
#include <carto/project/presentation_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace carto;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-presentation-catalog-" + std::to_string(stamp));
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

project::AuthoredCamera camera_source(scene::ObjectId object, bool active = false) {
    project::AuthoredCamera camera;
    camera.object = object;
    camera.identity = "review.motel-sign";
    camera.bookmark_identity = "motel-sign-approach";
    camera.reference_image_identity = "reference.sign-front";
    camera.projection = project::CameraProjection::perspective;
    camera.review_purpose = project::CameraReviewPurpose::silhouette_review;
    camera.vertical_fov_radians = 0.82;
    camera.near_plane = 0.05;
    camera.far_plane = 800.0;
    camera.active_for_review = active;
    camera.revision = core::Revision{2U};
    return camera;
}

project::AuthoredLight light_source(scene::ObjectId object) {
    project::AuthoredLight light;
    light.object = object;
    light.identity = "fixtures.exterior-sconce";
    light.semantic_class = "practical";
    light.fixture_owner = "motel-building";
    light.power_owner = "service-panel";
    light.switch_owner = "front-desk-switch";
    light.kind = project::AuthoredLightKind::spot;
    light.linear_color = {1.0, 0.71, 0.42};
    light.intensity = 900.0;
    light.range = 18.0;
    light.inner_cone_radians = 0.24;
    light.outer_cone_radians = 0.52;
    light.color_temperature_kelvin = 2'700.0;
    light.normal_state = project::LightNormalState::switchable;
    light.failure_state = project::LightFailureState::intermittent;
    light.flicker_eligible = true;
    light.revision = core::Revision{4U};
    return light;
}

void typed_records_round_trip_and_reject_invalid_intent() {
    project::PresentationCatalog catalog;
    auto camera_result = catalog.insert_camera(camera_source(scene::ObjectId{1U}, true));
    if (!camera_result) std::cerr << "camera admission: " << camera_result.error().message << '\n';
    require(static_cast<bool>(camera_result), "valid authored camera source is admitted");
    auto light_result = catalog.insert_light(light_source(scene::ObjectId{2U}));
    if (!light_result) std::cerr << "light admission: " << light_result.error().message << '\n';
    require(static_cast<bool>(light_result), "valid authored light source is admitted");
    auto catalog_result = catalog.validate();
    if (!catalog_result) std::cerr << "catalog validation: " << catalog_result.error().message << '\n';
    require(static_cast<bool>(catalog_result), "complete presentation catalog validates");
    const auto encoded = catalog.serialize();
    const auto decoded = project::PresentationCatalog::deserialize(encoded);
    require(decoded && decoded.value().serialize() == encoded &&
            decoded.value().cameras().at(scene::ObjectId{1U}).review_purpose ==
                project::CameraReviewPurpose::silhouette_review &&
            decoded.value().lights().at(scene::ObjectId{2U}).color_temperature_kelvin ==
                2'700.0,
        "typed camera and fixture-owned light intents survive deterministic round trip");
    const auto digest = catalog.canonical_digest();
    const auto decoded_digest = decoded.value().canonical_digest();
    require(digest && decoded_digest && digest.value() == decoded_digest.value(),
        "presentation source digest is stable across reopen");

    auto invalid_camera = camera_source(scene::ObjectId{3U});
    invalid_camera.vertical_fov_radians = std::numeric_limits<double>::quiet_NaN();
    require(!invalid_camera.validate(), "non-finite camera projection is rejected");
    invalid_camera = camera_source(scene::ObjectId{3U});
    invalid_camera.identity = "../private-camera";
    require(!invalid_camera.validate(), "path-like camera identity is rejected");
    auto invalid_light = light_source(scene::ObjectId{4U});
    invalid_light.outer_cone_radians = 2.0;
    require(!invalid_light.validate(), "invalid spot-light cone is rejected");
    invalid_light = light_source(scene::ObjectId{4U});
    invalid_light.color_temperature_kelvin = 80'000.0;
    require(!invalid_light.validate(), "out-of-range light temperature is rejected");

    project::PresentationCatalog two_active;
    require(two_active.insert_camera(camera_source(scene::ObjectId{10U}, true)) &&
            two_active.insert_camera(camera_source(scene::ObjectId{11U}, true)),
        "individual camera records can be staged before catalog-level checks");
    require(!two_active.validate(), "multiple active review cameras are rejected");

    auto tampered = encoded;
    const auto text = tampered.find("fixtures.exterior-sconce");
    require(text != std::string::npos, "tamper fixture contains authored light identity");
    tampered.replace(text, std::string("fixtures.exterior-sconce").size(), "../external");
    require(!project::PresentationCatalog::deserialize(tampered),
        "hostile serialized identity is rejected after round trip");
    require(!project::PresentationCatalog::deserialize(encoded + "extra"),
        "trailing catalog data is rejected");
}

void project_transaction_binds_presentation_to_scene_and_journal() {
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "presentation.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "presentation-authoring-test");
    require(static_cast<bool>(transaction), "presentation transaction begins");
    const auto camera_object = transaction.value().create_object("Sign review camera");
    const auto light_object = transaction.value().create_object("Exterior sconce");
    require(camera_object && light_object, "camera and light scene objects enter the transaction");

    project::PresentationCatalog catalog;
    auto camera = camera_source(camera_object.value(), true);
    auto light = light_source(light_object.value());
    require(catalog.insert_camera(std::move(camera)) &&
            catalog.insert_light(std::move(light)) &&
            transaction.value().set_presentation_catalog(std::move(catalog)),
        "presentation source records bind to existing staged scene objects");
    const auto receipt = transaction.value().commit("presentation.catalog.persist");
    require(receipt && receipt.value().revision_after > receipt.value().revision_before,
        "presentation authoring commits through the project transaction boundary");

    const auto events = journal.read_all();
    require(events && events.value().size() == 1U &&
            events.value().front().event_type == "presentation.catalog.persist",
        "presentation commit emits one durable journal event");
    const auto reopened = project::ProjectDocument::deserialize(document.serialize());
    require(reopened && reopened.value().presentation_catalog().cameras().size() == 1U &&
            reopened.value().presentation_catalog().lights().size() == 1U,
        "project save/reopen preserves authored scene presentation");

    auto remove = project::ProjectTransaction::begin(
        document, journal, "presentation-authoring-test");
    require(remove && !remove.value().remove_object(camera_object.value()),
        "scene object cannot be deleted while authored camera data references it");
    require(static_cast<bool>(remove.value().rollback()), "rejected object deletion rolls back");

    project::PresentationCatalog dangling;
    require(static_cast<bool>(dangling.insert_camera(camera_source(scene::ObjectId{999U}))),
        "dangling camera fixture is locally valid");
    auto reject = project::ProjectTransaction::begin(
        document, journal, "presentation-authoring-test");
    require(reject && !reject.value().set_presentation_catalog(std::move(dangling)),
        "catalog admission rejects references outside the project scene");
    require(static_cast<bool>(reject.value().rollback()), "dangling catalog rejection rolls back");
    const auto events_after_rejection = journal.read_all();
    require(events_after_rejection && events_after_rejection.value().size() == 1U,
        "failed presentation mutations append no journal event");
}

void schema_v17_migrates_and_current_schema_requires_spatial_field_table() {
    project::ProjectDocument document;
    auto current = document.serialize();
    const std::string curve_table = "CURVE_PATHS 0\n";
    const auto curve_table_start = current.find(curve_table);
    require(curve_table_start != std::string::npos,
        "current-schema project contains its curve path table");
    auto missing_curve_table = current;
    missing_curve_table.erase(curve_table_start, curve_table.size());
    require(!project::ProjectDocument::deserialize(missing_curve_table),
        "current-schema projects cannot omit the curve path table");

    const std::string record_prefix = "PRESENTATION_CATALOG ";
    const auto record_start = current.find(record_prefix);
    const auto project_end = current.rfind("\nEND\n");
    require(record_start != std::string::npos && project_end != std::string::npos &&
            project_end > record_start,
        "current-schema project contains its presentation record");
    auto missing = current;
    missing.erase(record_start, project_end - record_start + 1U);
    require(!project::ProjectDocument::deserialize(missing),
        "current-schema projects cannot omit the presentation catalog record");

    auto v17 = current;
    const auto spatial_table = v17.find("SPATIAL_FIELDS ");
    require(spatial_table != std::string::npos,
        "schema migration fixture can isolate current spatial-field records");
    v17 = v17.substr(0U, spatial_table) + "END\n";
    const std::string current_header = "CARTOGRAPHER_PROJECT 20";
    require(v17.starts_with(current_header), "schema migration fixture finds v20 header");
    v17.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 17");
    const auto migrated_v17 = project::ProjectDocument::deserialize(v17);
    require(migrated_v17 && migrated_v17.value().spatial_fields().empty() &&
            migrated_v17.value().road_sources().empty(),
        "schema-v17 projects migrate with no fabricated spatial fields");

    auto v16 = v17;
    const auto road_table = v16.find("ROAD_SOURCES ");
    require(road_table != std::string::npos,
        "schema migration fixture can isolate v17 road records");
    v16.erase(road_table);
    v16 += "END\n";
    v16.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 16");
    const auto migrated_v16 = project::ProjectDocument::deserialize(v16);
    require(migrated_v16 && migrated_v16.value().curve_paths().empty() &&
            migrated_v16.value().road_sources().empty(),
        "schema-v16 projects migrate with no invented road source table");

    auto v15 = v16;
    const auto v15_curve_table = v15.find("CURVE_PATHS ");
    require(v15_curve_table != std::string::npos,
        "schema-v15 migration fixture contains the v16-only curve table");
    v15.erase(v15_curve_table);
    v15 += "END\n";
    v15.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 15");
    const auto migrated_v15 = project::ProjectDocument::deserialize(v15);
    require(migrated_v15 && migrated_v15.value().presentation_catalog().empty(),
        "schema-v15 projects migrate with an explicit empty presentation catalog");

    auto v14 = missing;
    v14.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 14");
    const auto migrated_v14 = project::ProjectDocument::deserialize(v14);
    if (!migrated_v14) std::cerr << "v14 migration: " << migrated_v14.error().message << '\n';
    require(migrated_v14 && migrated_v14.value().presentation_catalog().empty(),
        "schema-v14 projects migrate with an explicit empty presentation catalog");
}

} // namespace

int main() {
    typed_records_round_trip_and_reject_invalid_intent();
    project_transaction_binds_presentation_to_scene_and_journal();
    schema_v17_migrates_and_current_schema_requires_spatial_field_table();
    std::cout << "presentation catalog tests passed\n";
    return EXIT_SUCCESS;
}
