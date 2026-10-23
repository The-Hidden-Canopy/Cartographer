#include <carto/journal/journal.hpp>
#include <carto/project/presentation_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <utility>
#include <span>
#include <vector>

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
    TempDirectory()
        : TempDirectory(std::filesystem::temp_directory_path(),
            "cartographer-presentation-catalog-") {}

    TempDirectory(const std::filesystem::path& root, std::string prefix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = root / (std::move(prefix) + std::to_string(stamp));
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

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

project::ReferenceImagePlane reference_plane_source(
    std::string identity,
    project::ReferencePlaneView view,
    std::string view_set_identity = {},
    std::optional<scene::ObjectId> pinned_camera = std::nullopt) {
    project::ReferenceImagePlane plane;
    plane.identity = std::move(identity);
    plane.source_asset_identity = "source." + plane.identity;
    plane.source_locator = "References/" + plane.identity + ".png";
    plane.source_digest = digest_text(plane.identity + ".image-bytes");
    plane.source_generation = core::Revision{1U};
    plane.image_width_pixels = 100U;
    plane.image_height_pixels = 50U;
    plane.view = view;
    plane.view_set_identity = std::move(view_set_identity);
    plane.pinned_camera = pinned_camera;
    plane.origin = {1.0, 2.0, 3.0};
    plane.width_meters = 10.0;
    plane.opacity = 0.7;
    plane.visible = true;
    plane.locked = false;
    plane.revision = core::Revision{1U};
    return plane;
}

assets::SourceImportRecord image_source_record(
    const project::ReferenceImagePlane& plane) {
    assets::SourceImportRecord record;
    record.identity = plane.source_asset_identity;
    record.relative_source_path = plane.source_locator;
    record.source_generation = plane.source_generation;
    record.kind = assets::SourceAssetKind::image;
    record.source_digest = plane.source_digest;
    record.importer_identity = "carto.import.image";
    record.importer_version = 1U;
    record.artifact_identity = "image." + plane.identity;
    record.artifact_digest = plane.source_digest;
    return record;
}

std::vector<std::uint8_t> one_pixel_tga() {
    std::vector<std::uint8_t> bytes(21U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = 1U;
    bytes[14U] = 1U;
    bytes[16U] = 24U;
    bytes[18U] = 0x30U;
    bytes[19U] = 0x60U;
    bytes[20U] = 0x90U;
    return bytes;
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

void reference_image_planes_are_bounded_calibrated_and_fail_closed() {
    project::PresentationCatalog catalog;
    const auto camera = camera_source(scene::ObjectId{40U});
    require(static_cast<bool>(catalog.insert_camera(camera)),
        "camera-pinned plane fixture has an authored camera");

    auto front = reference_plane_source(
        "motel.front", project::ReferencePlaneView::front, "motel.orthographic-set");
    front.calibration = project::ReferenceImageCalibration{
        {0.0, 0.0}, {10.0, 0.0}, 2.0};
    const auto calibrated_width = front.calibration->calibrated_width_meters(
        front.image_width_pixels, front.image_height_pixels);
    require(calibrated_width && calibrated_width.value() == 20.0,
        "known pixel distance calibrates image width into project meters");
    front.width_meters = calibrated_width.value();
    auto side = reference_plane_source(
        "motel.side", project::ReferencePlaneView::side, "motel.orthographic-set");
    auto top = reference_plane_source(
        "motel.top", project::ReferencePlaneView::top, "motel.orthographic-set");
    top.normal = {0.0, -1.0, 0.0};
    top.up = {0.0, 0.0, 1.0};
    auto camera_pinned = reference_plane_source(
        "motel.camera", project::ReferencePlaneView::camera_pinned, {}, camera.object);
    auto perspective = reference_plane_source(
        "motel.perspective", project::ReferencePlaneView::perspective);
    require(catalog.insert_reference_plane(front) &&
            catalog.insert_reference_plane(side) &&
            catalog.insert_reference_plane(top) &&
            catalog.insert_reference_plane(camera_pinned) &&
            catalog.insert_reference_plane(perspective),
        "front, side, top, perspective, and camera-pinned planes are admitted");
    require(static_cast<bool>(catalog.validate()),
        "complete view set and camera binding validate");

    const auto top_corners = project::reference_image_plane_corners(top);
    require(top_corners && top_corners.value().upper_left.x == -4.0 &&
            top_corners.value().upper_left.z == 5.5 &&
            top_corners.value().upper_right.x == 6.0 &&
            top_corners.value().upper_right.z == 5.5 &&
            top_corners.value().lower_right.x == 6.0 &&
            top_corners.value().lower_right.z == 0.5 &&
            top_corners.value().lower_left.x == -4.0 &&
            top_corners.value().lower_left.z == 0.5,
        "reference plane origin centers the image quad and pixel aspect defines its height");
    const auto top_plan_corners = project::reference_image_plane_xz_corners(top);
    require(top_plan_corners &&
            top_plan_corners.value().upper_left.x == top_corners.value().upper_left.x &&
            top_plan_corners.value().upper_left.y == top_corners.value().upper_left.z &&
            top_plan_corners.value().lower_right.x == top_corners.value().lower_right.x &&
            top_plan_corners.value().lower_right.y == top_corners.value().lower_right.z,
        "X/Z plan projection preserves top-plane world placement and image orientation");
    require(!project::reference_image_plane_xz_corners(front),
        "front references are not flattened into the X/Z plan work surface");

    auto out_of_bounds = top;
    out_of_bounds.origin.x = 1.0e9;
    require(!project::reference_image_plane_corners(out_of_bounds),
        "reference plane geometry rejects extents outside project placement bounds");

    const auto encoded = catalog.serialize();
    const auto reopened = project::PresentationCatalog::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded &&
            reopened.value().reference_planes().size() == 5U &&
            reopened.value().reference_planes().at("motel.front").width_meters == 20.0 &&
            reopened.value().reference_planes().at("motel.camera").pinned_camera == camera.object,
        "source digest, calibration, placement, visibility, lock, and view binding round-trip");
    const auto first_digest = catalog.canonical_digest();
    const auto reopened_digest = reopened.value().canonical_digest();
    require(first_digest && reopened_digest && first_digest.value() == reopened_digest.value(),
        "reference-plane catalog digest is stable across save/reopen");

    struct CommaDecimal final : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
    };
    const auto prior_locale = std::locale();
    std::locale::global(std::locale(prior_locale, new CommaDecimal));
    const std::string locale_encoded = catalog.serialize();
    std::locale::global(prior_locale);
    require(locale_encoded == encoded &&
            project::PresentationCatalog::deserialize(locale_encoded),
        "canonical reference-plane encoding is independent of the process decimal locale");

    auto invalid = front;
    invalid.source_locator = "../outside.png";
    require(!invalid.validate(), "traversing image locators are rejected");
    invalid = front;
    invalid.source_locator = "References/CON.png";
    require(!invalid.validate(), "Windows device-name image locators are rejected");
    invalid = front;
    invalid.opacity = 1.01;
    require(!invalid.validate(), "reference-plane opacity is bounded to [0, 1]");
    invalid = front;
    invalid.normal = {0.0, 0.0, 2.0};
    require(!invalid.validate(), "non-unit reference-plane orientation is rejected");
    invalid = front;
    invalid.calibration->second_pixel.x = static_cast<double>(invalid.image_width_pixels);
    require(!invalid.validate(), "calibration anchors outside the decoded image are rejected");
    invalid = front;
    invalid.source_digest = {};
    require(!invalid.validate(), "zero source digests are rejected");

    project::PresentationCatalog incomplete_set;
    require(static_cast<bool>(incomplete_set.insert_reference_plane(side)),
        "a partial view set can be staged before catalog admission");
    require(!incomplete_set.validate(),
        "project admission refuses a front/side/top set with missing views");
    project::PresentationCatalog missing_camera;
    require(static_cast<bool>(missing_camera.insert_reference_plane(reference_plane_source(
        "orphan.camera", project::ReferencePlaneView::camera_pinned, {},
        scene::ObjectId{999U}))), "camera-pinned record can be staged for admission");
    require(!missing_camera.validate(),
        "camera-pinned planes cannot resolve to missing authored cameras");
    project::PresentationCatalog duplicate_view;
    require(duplicate_view.insert_reference_plane(front) &&
            duplicate_view.insert_reference_plane(reference_plane_source(
                "motel.front.copy", project::ReferencePlaneView::front,
                "motel.orthographic-set")) &&
            duplicate_view.insert_reference_plane(side) &&
            duplicate_view.insert_reference_plane(top),
        "duplicate-view fixture can be staged");
    require(!duplicate_view.validate(), "a view set cannot contain two front planes");

    assets::ProjectAssetRegistry registry;
    const auto source = image_source_record(front);
    require(source.validate() && registry.set_record(source),
        "project image source registry accepts exact image lineage");
    require(project::inspect_reference_image_source(front, registry, std::nullopt) ==
                project::ReferenceImageSourceState::missing,
        "unreadable image bytes remain explicitly missing with no fallback");
    auto stale_generation = front;
    stale_generation.source_generation = core::Revision{2U};
    require(project::inspect_reference_image_source(
                stale_generation, registry, front.source_digest) ==
                project::ReferenceImageSourceState::stale,
        "a registry generation change invalidates the bound reference plane");
    require(project::inspect_reference_image_source(front, registry, front.source_digest) ==
                project::ReferenceImageSourceState::verified,
        "exact registered source and observed digest verify");
    require(project::inspect_reference_image_source(
                front, registry, digest_text("replacement image")) ==
                project::ReferenceImageSourceState::stale,
        "changed image bytes are reported stale instead of substituted");
    assets::ProjectAssetRegistry empty_registry;
    require(project::inspect_reference_image_source(
                front, empty_registry, front.source_digest) ==
                project::ReferenceImageSourceState::missing,
        "removed source registration is reported missing without changing the plane");
    auto wrong_kind = source;
    wrong_kind.kind = assets::SourceAssetKind::texture;
    assets::ProjectAssetRegistry wrong_kind_registry;
    require(wrong_kind_registry.set_record(wrong_kind) &&
            project::inspect_reference_image_source(
                front, wrong_kind_registry, front.source_digest) ==
                project::ReferenceImageSourceState::stale,
        "a non-image source cannot satisfy a reference-plane binding");
}

void reference_image_source_reads_only_verified_project_blobs() {
    // Keep this content-addressed fixture under the current build/test root:
    // BlobStore's atomic temporary filename otherwise exceeds legacy Windows
    // path limits when appended to the sandbox's long system temp path.
    TempDirectory temp(std::filesystem::current_path(), "pc-");
    const auto bytes = one_pixel_tga();
    assets::BlobStore store(temp.path() / ".carto-assets" / "source-blobs");
    const auto stored = store.put(
        std::span<const std::uint8_t>(bytes), "image/x-tga");
    if (!stored) {
        std::cerr << "image blob fixture: " << stored.error().message << '\n';
    }
    require(static_cast<bool>(stored), "valid image bytes enter the bounded blob store");

    auto plane = reference_plane_source(
        "resolved.image", project::ReferencePlaneView::perspective);
    plane.source_digest = stored.value().digest;
    plane.source_locator = store.path_for(stored.value().digest)
        .lexically_relative(temp.path()).generic_string();
    plane.image_width_pixels = 1U;
    plane.image_height_pixels = 1U;
    auto source = image_source_record(plane);
    assets::ProjectAssetRegistry registry;
    require(source.validate() && registry.set_record(source),
        "project registry binds the image identity, generation, locator, and digest");

    const auto loaded = project::read_reference_image_source_bytes(
        temp.path(), plane, registry);
    require(loaded && loaded.value() == bytes,
        "resolver returns exact digest-verified project-local image bytes");

    assets::ProjectAssetRegistry missing_registry;
    const auto missing = project::read_reference_image_source_bytes(
        temp.path(), plane, missing_registry);
    require(!missing && missing.error().code == core::ErrorCode::not_found,
        "missing source registration fails without fallback bytes");

    auto stale_plane = plane;
    stale_plane.source_generation = core::Revision{2U};
    const auto stale = project::read_reference_image_source_bytes(
        temp.path(), stale_plane, registry);
    require(!stale && stale.error().code == core::ErrorCode::stale_data,
        "stale source generations fail before bytes are returned");

    auto wrong_dimensions = plane;
    wrong_dimensions.image_width_pixels = 2U;
    const auto dimension_mismatch = project::read_reference_image_source_bytes(
        temp.path(), wrong_dimensions, registry);
    require(!dimension_mismatch &&
            dimension_mismatch.error().code == core::ErrorCode::stale_data,
        "source header dimensions must match the plane binding");

    auto traversing = plane;
    traversing.source_locator = "../outside.tga";
    const auto traversal = project::read_reference_image_source_bytes(
        temp.path(), traversing, registry);
    require(!traversal,
        "path traversal is rejected before resolving project-local bytes");

    const auto blob_path = store.path_for(stored.value().digest);
    std::error_code error;
    std::filesystem::remove(blob_path, error);
    require(!error, "blob fixture can be removed before corruption case");
    const auto missing_bytes = project::read_reference_image_source_bytes(
        temp.path(), plane, registry);
    require(!missing_bytes &&
            missing_bytes.error().code == core::ErrorCode::not_found,
        "missing project-local bytes remain missing with no fallback");
    {
        std::ofstream corrupted(blob_path, std::ios::binary | std::ios::trunc);
        corrupted.write("not-an-image", 12);
        require(static_cast<bool>(corrupted), "corrupt blob fixture is written");
    }
    const auto corrupt = project::read_reference_image_source_bytes(
        temp.path(), plane, registry);
    require(!corrupt,
        "content-addressed bytes with a digest mismatch are never returned");

    TempDirectory outside(std::filesystem::current_path(), "pc-outside-");
    const auto outside_image = outside.path() / "outside.tga";
    {
        std::ofstream image(outside_image, std::ios::binary | std::ios::trunc);
        image.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(image), "outside image fixture is written");
    }
    std::filesystem::remove(blob_path, error);
    require(!error, "corrupt in-project blob is removed before symlink case");
    std::filesystem::create_symlink(outside_image, blob_path, error);
    if (!error) {
        const auto escaped = project::read_reference_image_source_bytes(
            temp.path(), plane, registry);
        require(!escaped && escaped.error().code == core::ErrorCode::validation_failed,
            "a project blob symlink resolving outside the project is refused");
    } else {
        std::cout << "SKIP: symlink escape case unavailable: " << error.message() << '\n';
    }
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
    const auto front = reference_plane_source(
        "project.front", project::ReferencePlaneView::front, "project.orthos");
    const auto side = reference_plane_source(
        "project.side", project::ReferencePlaneView::side, "project.orthos");
    const auto top = reference_plane_source(
        "project.top", project::ReferencePlaneView::top, "project.orthos");
    const auto camera_pinned = reference_plane_source(
        "project.camera", project::ReferencePlaneView::camera_pinned, {}, camera_object.value());
    require(catalog.insert_camera(std::move(camera)) &&
            catalog.insert_light(std::move(light)) &&
            catalog.insert_reference_plane(front) &&
            catalog.insert_reference_plane(side) &&
            catalog.insert_reference_plane(top) &&
            catalog.insert_reference_plane(camera_pinned) &&
            transaction.value().set_presentation_catalog(std::move(catalog)),
        "presentation source records and camera-pinned planes bind to staged scene objects");
    const auto receipt = transaction.value().commit("presentation.catalog.persist");
    require(receipt && receipt.value().revision_after > receipt.value().revision_before,
        "presentation authoring commits through the project transaction boundary");

    const auto events = journal.read_all();
    require(events && events.value().size() == 1U &&
            events.value().front().event_type == "presentation.catalog.persist",
        "presentation commit emits one durable journal event");
    const auto reopened = project::ProjectDocument::deserialize(document.serialize());
    require(reopened && reopened.value().presentation_catalog().cameras().size() == 1U &&
            reopened.value().presentation_catalog().lights().size() == 1U &&
            reopened.value().presentation_catalog().reference_planes().size() == 4U &&
            project::inspect_reference_image_source(
                reopened.value().presentation_catalog().reference_planes().at("project.front"),
                reopened.value().source_asset_registry(), std::nullopt) ==
                project::ReferenceImageSourceState::missing,
        "project save/reopen preserves planes and reports unavailable sources without fallback");

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
    const std::string v1_catalog =
        "CARTOGRAPHER_PRESENTATION_CATALOG 1\nCAMERAS 0\nLIGHTS 0\nEND\n";
    const auto migrated_catalog = project::PresentationCatalog::deserialize(v1_catalog);
    require(migrated_catalog && migrated_catalog.value().reference_planes().empty() &&
            migrated_catalog.value().serialize().find(
                "CARTOGRAPHER_PRESENTATION_CATALOG 2\n") == 0U,
        "schema-v1 presentation catalogs migrate with an explicit empty plane table");
    const std::string v2_without_planes =
        "CARTOGRAPHER_PRESENTATION_CATALOG 2\nCAMERAS 0\nLIGHTS 0\nEND\n";
    require(!project::PresentationCatalog::deserialize(v2_without_planes),
        "schema-v2 catalogs cannot omit their reference-plane table");

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
    const auto mesh_patches = v17.find("MESH_PATCHES 0\n");
    require(mesh_patches != std::string::npos,
        "schema v17 fixture removes the v29 persistent patch table");
    v17.erase(mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const auto spatial_table = v17.find("SPATIAL_FIELDS ");
    require(spatial_table != std::string::npos,
        "schema migration fixture can isolate current spatial-field records");
    v17 = v17.substr(0U, spatial_table) + "END\n";
    const auto sweep_table = v17.find("SWEEP_SOURCES 0\n");
    require(sweep_table != std::string::npos,
        "schema v17 fixture removes the v22 sweep-source table");
    v17.erase(sweep_table, std::string("SWEEP_SOURCES 0\n").size());
    const auto sign_text_table = v17.find("SIGN_TEXT_SOURCES 0\n");
    require(sign_text_table != std::string::npos,
        "schema v17 fixture removes the schema-v23 sign-text table");
    v17.erase(sign_text_table, std::string("SIGN_TEXT_SOURCES 0\n").size());
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion);
    require(v17.starts_with(current_header), "schema migration fixture finds current header");
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
    const auto v14_mesh_patches = v14.find("MESH_PATCHES 0\n");
    require(v14_mesh_patches != std::string::npos,
        "schema v14 fixture removes the v29 persistent patch table");
    v14.erase(v14_mesh_patches, std::string("MESH_PATCHES 0\n").size());
    v14.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 14");
    const auto migrated_v14 = project::ProjectDocument::deserialize(v14);
    if (!migrated_v14) std::cerr << "v14 migration: " << migrated_v14.error().message << '\n';
    require(migrated_v14 && migrated_v14.value().presentation_catalog().empty(),
        "schema-v14 projects migrate with an explicit empty presentation catalog");
}

} // namespace

int main() {
    typed_records_round_trip_and_reject_invalid_intent();
    reference_image_planes_are_bounded_calibrated_and_fail_closed();
    reference_image_source_reads_only_verified_project_blobs();
    project_transaction_binds_presentation_to_scene_and_journal();
    schema_v17_migrates_and_current_schema_requires_spatial_field_table();
    std::cout << "presentation catalog tests passed\n";
    return EXIT_SUCCESS;
}
