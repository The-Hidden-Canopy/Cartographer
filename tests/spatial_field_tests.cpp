#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include <carto/spatial_field/scalar_field.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

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
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-spatial-field-tests-" + std::to_string(stamp));
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

spatial_field::ScalarField2D make_field() {
    auto created = spatial_field::ScalarField2D::create(
        "field.pine-ridge.moisture",
        spatial_field::FieldSemantic::moisture,
        {100.0, 200.0}, 1.0, 5U, 5U, 0.0, 1.0, 0.0);
    require(static_cast<bool>(created), "bounded scalar field creates");
    return std::move(created.value());
}

void scalar_field_is_canonical_bounded_and_revision_checked() {
    auto field = make_field();
    require(static_cast<bool>(field.validate()), "new source field validates");
    const auto encoded = field.serialize();
    const auto reopened = spatial_field::ScalarField2D::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "source field has a deterministic canonical round trip");
    const auto original_digest = field.canonical_digest();
    const auto reopened_digest = reopened.value().canonical_digest();
    require(original_digest && reopened_digest && original_digest.value() == reopened_digest.value(),
        "canonical field digest is stable across reopen");
    auto legacy_field_record = encoded;
    const std::string current_header = "CARTOGRAPHER_SCALAR_FIELD_2D " +
        std::to_string(spatial_field::ScalarField2D::kSchemaVersion);
    require(legacy_field_record.find(current_header) == 0U,
        "writer emits the current scalar-field schema version");
    legacy_field_record.replace(0U, current_header.size(), "CARTOGRAPHER_SCALAR_FIELD_2D 1");
    require(static_cast<bool>(spatial_field::ScalarField2D::deserialize(legacy_field_record)),
        "legacy scalar-field v1 records remain readable for existing semantics");

    spatial_field::FieldBrush paint;
    paint.expected_revision = core::Revision{1U};
    paint.center_uv = {102.0, 202.0};
    paint.radius = 2.1;
    paint.value = 0.8;
    paint.mode = spatial_field::BrushMode::replace;
    const auto dirty = field.paint(paint);
    require(dirty && dirty.value().changed &&
            dirty.value().previous_revision == core::Revision{1U} &&
            dirty.value().previous_digest == original_digest.value() &&
            dirty.value().resulting_revision == core::Revision{2U} &&
            dirty.value().source_digest == field.canonical_digest().value() &&
            dirty.value().min_u == 0U && dirty.value().max_u == 4U,
        "brush edit advances one source revision and reports a bounded dirty region");
    const auto center = field.sample_bilinear({102.0, 202.0});
    require(center && std::abs(center.value() - 0.8) < 1.0e-12,
        "field sampling reads the authored brush result");
    const auto edge = field.sample_bilinear({104.0, 204.0});
    require(edge && edge.value() == 0.0, "radial falloff leaves samples outside the brush unchanged");

    const auto before_stale = field.serialize();
    paint.expected_revision = core::Revision{1U};
    require(!field.paint(paint) && field.serialize() == before_stale,
        "stale paint is refused without changing field bytes");
    require(!field.sample_bilinear({99.0, 202.0}),
        "sampling outside the authored grid fails instead of inventing fallback values");

    auto no_op = make_field();
    spatial_field::FieldBrush unchanged;
    unchanged.expected_revision = core::Revision{1U};
    unchanged.center_uv = {102.0, 202.0};
    unchanged.radius = 1.0;
    unchanged.value = 0.0;
    const auto no_op_receipt = no_op.paint(unchanged);
    require(no_op_receipt && !no_op_receipt.value().changed &&
            no_op.source_revision() == core::Revision{1U},
        "no-op paint does not advance source revision");

    auto noisy_a = make_field();
    spatial_field::FieldBrush peak;
    peak.expected_revision = core::Revision{1U};
    peak.center_uv = {102.0, 202.0};
    peak.radius = 1.0;
    peak.value = 1.0;
    require(static_cast<bool>(noisy_a.paint(peak)), "smoothing fixture can seed an impulse");
    auto noisy_b = noisy_a;
    spatial_field::FieldBrush smooth;
    smooth.expected_revision = core::Revision{2U};
    smooth.center_uv = {102.0, 202.0};
    smooth.radius = 2.1;
    smooth.mode = spatial_field::BrushMode::smooth;
    require(static_cast<bool>(noisy_a.paint(smooth)) &&
            static_cast<bool>(noisy_b.paint(smooth)),
        "smoothing applies against a valid source revision");
    require(noisy_a.serialize() == noisy_b.serialize() &&
            noisy_a.sample_bilinear({102.0, 202.0}).value() < 1.0,
        "smoothing is repeatable and reads an immutable pre-stroke snapshot");

    auto malformed = encoded;
    const auto version = malformed.find(current_header);
    require(version != std::string::npos, "field fixture has a schema marker");
    malformed.replace(version, current_header.size(),
        "CARTOGRAPHER_SCALAR_FIELD_2D 7");
    require(!spatial_field::ScalarField2D::deserialize(malformed),
        "unsupported scalar-field schema fails closed");
    require(!spatial_field::ScalarField2D::deserialize(encoded + "unexpected"),
        "unexpected trailing data is rejected");

    auto terrain_semantic_in_legacy_schema = spatial_field::ScalarField2D::create(
        "terrain.tile.x0.z0", spatial_field::FieldSemantic::terrain_elevation,
        {0.0, 0.0}, 1.0, 2U, 2U, 0.0, 1.0, 0.0);
    require(static_cast<bool>(terrain_semantic_in_legacy_schema),
        "new terrain semantic creates in the extended field schema");
    auto mislabeled_legacy = terrain_semantic_in_legacy_schema.value().serialize();
    mislabeled_legacy.replace(0U, current_header.size(), "CARTOGRAPHER_SCALAR_FIELD_2D 1");
    require(!spatial_field::ScalarField2D::deserialize(mislabeled_legacy),
        "new semantics cannot masquerade as legacy schema-v1 data");

    auto out_of_range = make_field();
    spatial_field::FieldBrush add;
    add.expected_revision = core::Revision{1U};
    add.center_uv = {102.0, 202.0};
    add.radius = 1.5;
    add.value = 1.0;
    add.mode = spatial_field::BrushMode::add;
    require(static_cast<bool>(out_of_range.paint(add)),
        "field paint can add inside its declared range");
    require(std::all_of(out_of_range.values().begin(), out_of_range.values().end(),
            [](double value) { return value >= 0.0 && value <= 1.0; }),
        "field brush clamps all derived samples to the declared range");
}

void project_field_mutations_are_journaled_and_migrate_v17() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "spatial-field-events.log");

    auto create = project::ProjectTransaction::begin(document, journal, "field-authoring-test");
    require(create && create.value().set_spatial_field(make_field()),
        "new field enters through a staged project transaction");
    const auto created = create.value().commit("spatial-field.create");
    require(created && document.spatial_fields().size() == 1U &&
            created.value().revision_after > created.value().revision_before,
        "new field is published only with a project revision transition");

    auto revised = document.spatial_fields().at("field.pine-ridge.moisture");
    spatial_field::FieldBrush brush;
    brush.expected_revision = revised.source_revision();
    brush.center_uv = {102.0, 202.0};
    brush.radius = 2.0;
    brush.value = 0.6;
    require(static_cast<bool>(revised.paint(brush)),
        "current source can be revised before staging");
    auto update = project::ProjectTransaction::begin(document, journal, "field-authoring-test");
    require(update && update.value().set_spatial_field(std::move(revised)),
        "next source revision can be staged");
    require(static_cast<bool>(update.value().commit("spatial-field.paint")),
        "source edit journals and publishes atomically");

    const auto events = journal.read_all();
    require(events && events.value().size() == 2U &&
            events.value()[0U].event_type == "spatial-field.create" &&
            events.value()[1U].event_type == "spatial-field.paint",
        "only successful field mutations append journal events");
    const auto reopened = project::ProjectDocument::deserialize(document.serialize());
    require(reopened && reopened.value().spatial_fields().size() == 1U &&
            reopened.value().spatial_fields().at("field.pine-ridge.moisture").source_revision() ==
                core::Revision{2U},
        "project save/reopen preserves field source identity and revision");
    std::string legacy_v18 = document.serialize();
    const auto sweep_table = legacy_v18.find("SWEEP_SOURCES 0\n");
    require(sweep_table != std::string::npos,
        "schema v18 fixture removes the v22 sweep-source table");
    legacy_v18.erase(sweep_table, std::string("SWEEP_SOURCES 0\n").size());
    const auto lineage_table = legacy_v18.find("SPATIAL_FIELD_LINEAGE ");
    require(lineage_table != std::string::npos,
        "current project includes retained spatial-field lineage");
    legacy_v18 = legacy_v18.substr(0U, lineage_table) + "END\n";
    const std::string current_header = "CARTOGRAPHER_PROJECT 22";
    require(legacy_v18.starts_with(current_header),
        "schema v18 fixture starts with the current project header");
    legacy_v18.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 18");
    const auto migrated_v18 = project::ProjectDocument::deserialize(legacy_v18);
    require(migrated_v18 && migrated_v18.value().spatial_fields().size() == 1U &&
            migrated_v18.value().spatial_fields().at("field.pine-ridge.moisture")
                .source_revision() == core::Revision{2U},
        "schema v18 migration derives lineage only from its existing active field");
    const auto saved_path = directory.path() / "field.carto";
    const auto save_receipt = document.save_atomic(saved_path);
    if (!save_receipt) std::cerr << "save failed: " << save_receipt.error().message << '\n';
    require(static_cast<bool>(save_receipt), "field project saves atomically");
    const auto loaded = project::ProjectDocument::load(saved_path);
    require(loaded && loaded.value().spatial_fields().at("field.pine-ridge.moisture").serialize() ==
            document.spatial_fields().at("field.pine-ridge.moisture").serialize(),
        "disk reopen preserves exact field data");

    auto stale = project::ProjectTransaction::begin(document, journal, "field-authoring-test");
    require(stale && !stale.value().set_spatial_field(make_field()),
        "stale field source revision cannot overwrite current project state");
    require(static_cast<bool>(stale.value().rollback()), "stale field edit rolls back cleanly");
    const auto events_after_stale = journal.read_all();
    require(events_after_stale && events_after_stale.value().size() == 2U,
        "rejected stale mutation emits no journal event");

    auto erase = project::ProjectTransaction::begin(document, journal, "field-authoring-test");
    require(erase && erase.value().erase_spatial_field(
                "field.pine-ridge.moisture", core::Revision{2U}),
        "field deletion requires its exact current source revision");
    require(static_cast<bool>(erase.value().commit("spatial-field.erase")),
        "field deletion is journaled as a project transition");
    const auto erased_round_trip = project::ProjectDocument::deserialize(document.serialize());
    require(erased_round_trip && erased_round_trip.value().spatial_fields().empty() &&
            erased_round_trip.value().serialize().find(
                "SPATIAL_FIELD_LINEAGE_RECORD \"field.pine-ridge.moisture\" 2") !=
                    std::string::npos,
        "project round-trip preserves a deleted identity's source-revision tombstone");
    auto reused_old = project::ProjectTransaction::begin(document, journal, "field-authoring-test");
    require(reused_old && !reused_old.value().set_spatial_field(make_field()),
        "deleted field identity cannot be recreated at its stale revision");
    require(static_cast<bool>(reused_old.value().rollback()),
        "stale identity reuse rolls back cleanly");
    auto reborn = make_field().at_next_revision(core::Revision{2U});
    require(static_cast<bool>(reborn), "recreated source can advance retained lineage");
    auto recreate = project::ProjectTransaction::begin(document, journal, "field-authoring-test");
    require(recreate && recreate.value().set_spatial_field(reborn.value()),
        "identity can be reused only at the next retained source revision");
    require(static_cast<bool>(recreate.value().commit("spatial-field.recreate")) &&
            document.spatial_fields().at("field.pine-ridge.moisture").source_revision() ==
                core::Revision{3U},
        "recreated field advances lineage without reusing revision one");
    const auto events_after_lineage = journal.read_all();
    require(events_after_lineage && events_after_lineage.value().size() == 4U,
        "only successful create, paint, erase, and recreate transitions are journaled");

    auto current = document.serialize();
    const auto table = current.find("SPATIAL_FIELDS ");
    const auto lineage = current.find("SPATIAL_FIELD_LINEAGE ");
    const auto terminal = current.rfind("END\n");
    require(table != std::string::npos && lineage > table && terminal > lineage,
        "current project includes its required spatial-field and lineage tables");
    auto no_table = current.substr(0U, table) + "END\n";
    require(!project::ProjectDocument::deserialize(no_table),
        "current project cannot omit the mandatory spatial-field table");
    auto no_lineage = current.substr(0U, lineage) + "END\n";
    require(!project::ProjectDocument::deserialize(no_lineage),
        "schema v19 project cannot omit the mandatory field-lineage table");
    const auto v17_sweep_table = current.find("SWEEP_SOURCES 0\n");
    require(v17_sweep_table != std::string::npos,
        "schema v17 fixture removes the v22 sweep-source table");
    current.erase(v17_sweep_table, std::string("SWEEP_SOURCES 0\n").size());
    const std::string v17_current_header = "CARTOGRAPHER_PROJECT 22";
    current.replace(0U, v17_current_header.size(), "CARTOGRAPHER_PROJECT 17");
    const auto legacy_spatial_table = current.find("SPATIAL_FIELDS ");
    require(legacy_spatial_table != std::string::npos,
        "schema v17 fixture contains the current spatial-field table");
    current = current.substr(0U, legacy_spatial_table) + "END\n";
    const auto migrated = project::ProjectDocument::deserialize(current);
    require(migrated && migrated.value().spatial_fields().empty(),
        "schema v17 migration introduces no fabricated spatial fields");
}

void project_save_reopen_preserves_extended_terrain_field_schema() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "terrain-field-events.log");
    auto field = spatial_field::ScalarField2D::create(
        "terrain.tile.x-3.z8", spatial_field::FieldSemantic::terrain_elevation,
        {-300.0, 800.0}, 2.0, 4U, 3U, -100.0, 900.0, 125.0);
    require(static_cast<bool>(field), "terrain elevation source is valid for project insertion");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "terrain-field-persistence-test");
    require(transaction && transaction.value().set_spatial_field(field.value()),
        "extended terrain source stages through the governed project transaction");
    require(static_cast<bool>(transaction.value().commit("terrain.source.create")),
        "terrain source commit is journaled");
    const auto saved_path = directory.path() / "terrain-field.carto";
    require(static_cast<bool>(document.save_atomic(saved_path)),
        "terrain source project saves atomically");
    const auto reopened = project::ProjectDocument::load(saved_path);
    require(reopened && reopened.value().spatial_fields().size() == 1U &&
            reopened.value().spatial_fields().at(field.value().identity()).semantic() ==
                spatial_field::FieldSemantic::terrain_elevation &&
            reopened.value().spatial_fields().at(field.value().identity()).serialize() ==
                field.value().serialize(),
        "project save/reopen preserves v2 terrain semantics, values, and revision");
    const auto events = journal.read_all();
    require(events && events.value().size() == 1U &&
            events.value().front().event_type == "terrain.source.create",
        "project persistence does not bypass the domain journal event");
}

void wind_vector_preview_is_aligned_bounded_and_source_bound() {
    auto u = spatial_field::ScalarField2D::create(
        "field.wind.u", spatial_field::FieldSemantic::wind_u,
        {10.0, -4.0}, 0.5, 40U, 24U, -5.0, 5.0, 3.0);
    auto v = spatial_field::ScalarField2D::create(
        "field.wind.v", spatial_field::FieldSemantic::wind_v,
        {10.0, -4.0}, 0.5, 40U, 24U, -5.0, 5.0, -4.0);
    require(u && v, "aligned wind component sources are valid");
    const auto u_digest = u.value().canonical_digest();
    const auto v_digest = v.value().canonical_digest();
    require(u_digest && v_digest, "wind sources provide exact canonical digests");

    const auto preview = spatial_field::build_vector_field_preview(u.value(), v.value());
    require(preview && preview.value().validate(),
        "aligned wind sources produce a valid derived vector preview");
    require(preview.value().preview_width == 16U && preview.value().preview_height == 16U &&
            preview.value().samples.size() == spatial_field::VectorFieldPreview::kMaxSamples,
        "vector visualization decimates deterministically to a hard sample cap");
    require(preview.value().u_component.identity == "field.wind.u" &&
            preview.value().u_component.source_revision == core::Revision{1U} &&
            preview.value().u_component.digest == u_digest.value() &&
            preview.value().v_component.identity == "field.wind.v" &&
            preview.value().v_component.source_revision == core::Revision{1U} &&
            preview.value().v_component.digest == v_digest.value(),
        "preview binds both exact source identities, revisions, and digests");
    require(preview.value().sources_match("field.wind.u", core::Revision{1U},
                u_digest.value(), "field.wind.v", core::Revision{1U}, v_digest.value()),
        "current paired-source receipts match the generated preview");
    require(preview.value().samples.front().position_uv.x == 10.0 &&
            preview.value().samples.front().position_uv.y == -4.0 &&
            preview.value().samples.front().vector_uv.x == 3.0 &&
            preview.value().samples.front().vector_uv.y == -4.0 &&
            preview.value().samples.back().position_uv.x == 29.5 &&
            preview.value().samples.back().position_uv.y == 7.5,
        "sample vectors and decimated positions retain the source U/V frame");
    const auto repeated = spatial_field::build_vector_field_preview(u.value(), v.value());
    require(repeated && repeated.value().samples.size() == preview.value().samples.size() &&
            repeated.value().samples.front().vector_uv.x ==
                preview.value().samples.front().vector_uv.x &&
            repeated.value().samples.back().position_uv.y ==
                preview.value().samples.back().position_uv.y,
        "vector preview generation is deterministic");

    auto wrong_semantic = spatial_field::ScalarField2D::create(
        "field.wind.wrong", spatial_field::FieldSemantic::moisture,
        {10.0, -4.0}, 0.5, 40U, 24U, -5.0, 5.0, 0.0);
    auto shifted = spatial_field::ScalarField2D::create(
        "field.wind.shifted", spatial_field::FieldSemantic::wind_v,
        {10.25, -4.0}, 0.5, 40U, 24U, -5.0, 5.0, 0.0);
    auto different_grid = spatial_field::ScalarField2D::create(
        "field.wind.different-grid", spatial_field::FieldSemantic::wind_v,
        {10.0, -4.0}, 0.5, 39U, 24U, -5.0, 5.0, 0.0);
    require(wrong_semantic && shifted && different_grid,
        "adversarial vector source fixtures are individually valid scalar fields");
    require(!spatial_field::build_vector_field_preview(wrong_semantic.value(), v.value()) &&
            !spatial_field::build_vector_field_preview(u.value(), shifted.value()) &&
            !spatial_field::build_vector_field_preview(u.value(), different_grid.value()) &&
            !spatial_field::build_vector_field_preview(u.value(), v.value(), 1U) &&
            !spatial_field::build_vector_field_preview(u.value(), v.value(), 17U),
        "wrong semantics, mismatched grids, and invalid caps fail closed");

    auto edited_u = u.value();
    spatial_field::FieldBrush brush;
    brush.expected_revision = core::Revision{1U};
    brush.center_uv = {10.0, -4.0};
    brush.radius = 0.5;
    brush.value = 4.0;
    brush.mode = spatial_field::BrushMode::replace;
    require(static_cast<bool>(edited_u.paint(brush)),
        "source edit advances the wind component before rebuilding the preview");
    const auto refreshed = spatial_field::build_vector_field_preview(edited_u, v.value());
    require(refreshed &&
            refreshed.value().u_component.source_revision == core::Revision{2U} &&
            refreshed.value().u_component.digest != preview.value().u_component.digest &&
            preview.value().u_component.source_revision == core::Revision{1U} &&
            !preview.value().sources_match("field.wind.u", core::Revision{2U},
                refreshed.value().u_component.digest, "field.wind.v", core::Revision{1U},
                refreshed.value().v_component.digest),
        "later source edits do not mutate old preview receipts and alter new receipts");

    auto forged = preview.value();
    forged.samples.push_back({});
    require(!forged.validate(), "forged over-cap preview sample payload is rejected");
}

} // namespace

int main() {
    scalar_field_is_canonical_bounded_and_revision_checked();
    project_field_mutations_are_journaled_and_migrate_v17();
    project_save_reopen_preserves_extended_terrain_field_schema();
    wind_vector_preview_is_aligned_bounded_and_source_bound();
    std::cout << "spatial field tests passed\n";
    return EXIT_SUCCESS;
}
