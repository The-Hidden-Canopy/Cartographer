#include <carto/production/decal.hpp>
#include <carto/journal/journal.hpp>

#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>

namespace {

using carto::assets::Sha256Digest;
using carto::core::Revision;
using carto::core::Vec3d;
using carto::geometry::EditableMesh;
using carto::production::DecalPlacement;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double left, double right, double epsilon = 1e-9) {
    return std::abs(left - right) <= epsilon;
}

Sha256Digest test_digest(char digit) {
    auto digest = Sha256Digest::from_hex(std::string(64U, digit));
    require(static_cast<bool>(digest), "test digest parses");
    return digest.value();
}

EditableMesh make_triangle(double z = 0.0, bool reverse = false) {
    EditableMesh mesh;
    const auto a = mesh.add_vertex({-1.0, -1.0, z});
    const auto b = mesh.add_vertex({1.0, -1.0, z});
    const auto c = mesh.add_vertex({0.0, 1.0, z});
    require(a && b && c, "triangle vertices are accepted");
    const auto face = reverse
        ? mesh.add_face({a.value(), c.value(), b.value()})
        : mesh.add_face({a.value(), b.value(), c.value()});
    require(static_cast<bool>(face), "triangle face is accepted");
    require(static_cast<bool>(mesh.validate()), "triangle mesh validates");
    return mesh;
}

DecalPlacement make_placement(const EditableMesh& mesh) {
    DecalPlacement placement;
    placement.id = 7U;
    placement.revision = Revision(1U);
    placement.target_mesh_asset = 19U;
    placement.target_mesh_revision = mesh.revision();
    placement.target_mesh_digest = test_digest('a');
    placement.material_id = 31U;
    placement.center = {0.0, 0.0, 0.0};
    placement.normal = {0.0, 0.0, 1.0};
    placement.tangent = {1.0, 0.0, 0.0};
    placement.width = 2.0;
    placement.height = 2.0;
    placement.depth = 0.2;
    return placement;
}

class CommaDecimal final : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
};

class GlobalLocaleGuard final {
public:
    GlobalLocaleGuard() : previous_(std::locale()) {}
    ~GlobalLocaleGuard() { std::locale::global(previous_); }

private:
    std::locale previous_;
};

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-decal-tests-" + std::to_string(stamp));
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

void projection_is_bounded_and_keeps_source_correspondence() {
    const auto mesh = make_triangle();
    const auto placement = make_placement(mesh);
    const auto projection = carto::production::project_decal(
        placement, mesh, placement.target_mesh_asset, placement.target_mesh_digest);
    require(projection && projection.value().polygons.size() == 1U,
        "intersecting triangle projects into one polygon");
    const auto& polygon = projection.value().polygons.front();
    require(polygon.source_face && polygon.vertices.size() == 3U,
        "projection retains source face and triangle boundary");
    require(projection.value().material_id == placement.material_id &&
            projection.value().target_mesh_digest == placement.target_mesh_digest,
        "projection retains material and exact source binding");
    for (const auto& vertex : polygon.vertices) {
        require(vertex.uv.x >= 0.0 && vertex.uv.x <= 1.0 &&
                vertex.uv.y >= 0.0 && vertex.uv.y <= 1.0,
            "projected UVs stay inside the authored decal rectangle");
        const double barycentric_sum = vertex.source_barycentric[0U] +
            vertex.source_barycentric[1U] + vertex.source_barycentric[2U];
        require(near(barycentric_sum, 1.0),
            "projected vertex maps to a source-triangle barycentric point");
    }
    const auto repeated = carto::production::project_decal(
        placement, mesh, placement.target_mesh_asset, placement.target_mesh_digest);
    require(repeated && repeated.value().polygons.size() == projection.value().polygons.size() &&
            repeated.value().polygons.front().vertices.size() == polygon.vertices.size(),
        "projection topology is deterministic across identical inputs");
}

void stale_identity_and_projection_limits_fail_closed() {
    const auto mesh = make_triangle();
    const auto placement = make_placement(mesh);
    require(!carto::production::project_decal(
                placement, mesh, placement.target_mesh_asset + 1U,
                placement.target_mesh_digest),
        "different source asset is refused");
    require(!carto::production::project_decal(
                placement, mesh, placement.target_mesh_asset, test_digest('b')),
        "changed source digest is refused");
    require(!carto::production::validate_decal_source(
                placement, placement.target_mesh_asset,
                placement.target_mesh_revision.next(), placement.target_mesh_digest),
        "changed source revision is refused");

    auto invalid = placement;
    invalid.width = 0.0;
    require(!carto::production::project_decal(
                invalid, mesh, invalid.target_mesh_asset, invalid.target_mesh_digest),
        "zero projection width is refused before clipping");
    invalid = placement;
    invalid.height = DecalPlacement::kMaxExtent + 1.0;
    require(!invalid.validate(), "unbounded projection height is refused");

    auto backfacing = placement;
    backfacing.minimum_normal_dot = 0.9;
    const auto reversed_mesh = make_triangle(0.0, true);
    // Reversed geometry has a different revision; bind to its exact current
    // revision and digest so this case isolates the explicit normal policy.
    backfacing.target_mesh_revision = reversed_mesh.revision();
    const auto rejected_backface = carto::production::project_decal(
        backfacing, reversed_mesh, backfacing.target_mesh_asset,
        backfacing.target_mesh_digest);
    require(rejected_backface && rejected_backface.value().polygons.empty(),
        "normal threshold excludes back-facing source triangles");
}

void placement_revision_and_serialization_are_fail_closed() {
    const auto mesh = make_triangle();
    const auto placement = make_placement(mesh);
    auto desired = placement;
    desired.revision = placement.revision.next();
    desired.width = 3.0;
    const auto revised = carto::production::revise_decal_placement(
        placement, placement.revision, desired);
    require(revised && revised.value().revision == placement.revision.next() &&
            revised.value().width == 3.0,
        "valid placement edit advances one revision");
    require(!carto::production::revise_decal_placement(
                placement, placement.revision.next(), desired),
        "stale placement edit is refused");
    desired.revision = Revision(placement.revision.value() + 2U);
    require(!carto::production::revise_decal_placement(
                placement, placement.revision, desired),
        "skipped placement revision is refused");

    const auto encoded = carto::production::serialize_decal_placement(placement);
    require(static_cast<bool>(encoded), "valid placement serializes");
    const auto decoded = carto::production::deserialize_decal_placement(encoded.value());
    require(decoded && carto::production::serialize_decal_placement(decoded.value()).value() ==
            encoded.value(),
        "placement serialization round-trips deterministically");
    require(!carto::production::deserialize_decal_placement(encoded.value() + " trailing"),
        "trailing serialized data is refused");
}

void placement_serialization_is_locale_independent() {
    const auto mesh = make_triangle();
    auto placement = make_placement(mesh);
    placement.center.x = 1.25;
    placement.width = 2.5;
    const auto canonical = carto::production::serialize_decal_placement(placement);
    require(static_cast<bool>(canonical), "placement serializes before locale change");

    GlobalLocaleGuard restore_locale;
    std::locale::global(std::locale(std::locale(), new CommaDecimal));
    require(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() == ',',
        "adversarial comma-decimal process locale is active");
    const auto under_comma_locale = carto::production::serialize_decal_placement(placement);
    require(under_comma_locale && under_comma_locale.value() == canonical.value(),
        "canonical decimal encoding is independent of the process locale");
    const auto decoded = carto::production::deserialize_decal_placement(canonical.value());
    require(decoded && near(decoded.value().center.x, 1.25) &&
            near(decoded.value().width, 2.5),
        "decoder reads canonical decimals independently of the process locale");
}

void placement_project_transaction_persists_revision_and_journal() {
    TempDirectory directory;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(directory.path() / "decal-events.log");
    const auto mesh = make_triangle();
    const auto initial = make_placement(mesh);

    auto create = carto::project::ProjectTransaction::begin(
        document, journal, "decal-tests");
    require(create && carto::production::stage_decal_placement(
            create.value(), initial, Revision{}),
        "valid decal placement stages through the project transaction");
    const auto created = create.value().commit("decal.placement.create");
    require(created && created.value().revision_after == Revision{1U} &&
            document.decal_placements().at(initial.id).revision == Revision{1U},
        "decal creation advances project and placement revisions");

    auto stale = initial;
    stale.revision = Revision{2U};
    auto stale_update = carto::project::ProjectTransaction::begin(
        document, journal, "decal-tests");
    require(stale_update && !carto::production::stage_decal_placement(
            stale_update.value(), stale, Revision{}),
        "stale placement revision cannot overwrite the current record");
    require(static_cast<bool>(stale_update.value().rollback()),
        "stale decal transaction rolls back without a journal event");

    auto skipped = initial;
    skipped.revision = Revision{3U};
    auto skipped_update = carto::project::ProjectTransaction::begin(
        document, journal, "decal-tests");
    require(skipped_update && !carto::production::stage_decal_placement(
            skipped_update.value(), skipped, Revision{1U}),
        "skipped placement revision is refused");
    require(static_cast<bool>(skipped_update.value().rollback()),
        "invalid decal transition rolls back without a journal event");

    auto revised = initial;
    revised.revision = Revision{2U};
    revised.width = 3.0;
    auto update = carto::project::ProjectTransaction::begin(
        document, journal, "decal-tests");
    require(update && carto::production::stage_decal_placement(
            update.value(), revised, Revision{1U}),
        "next decal revision stages against the current project record");
    require(static_cast<bool>(update.value().commit("decal.placement.update")),
        "decal update is journaled and published");

    const auto serialized_project = document.serialize();
    const auto reopened = carto::project::ProjectDocument::deserialize(serialized_project);
    require(reopened && reopened.value().schema_version() ==
            carto::project::ProjectDocument::kSchemaVersion,
        "current project schema round-trips the decal placement table");
    const auto decoded = carto::production::load_decal_placement(
        reopened.value(), revised.id);
    require(decoded && decoded.value().revision == Revision{2U} &&
            near(decoded.value().width, 3.0),
        "production loader verifies stored placement identity and revision");
    std::string tampered = serialized_project;
    const auto placement_record = tampered.find("DECAL_PLACEMENT 7 2 ");
    require(placement_record != std::string::npos,
        "serialized placement record is located for digest tampering");
    tampered.replace(placement_record + std::string("DECAL_PLACEMENT 7 2 ").size(),
        64U, std::string(64U, 'b'));
    require(!carto::project::ProjectDocument::deserialize(tampered),
        "tampered placement digest fails project admission");
    const auto events = journal.read_all();
    require(events && events.value().size() == 2U &&
            events.value()[0U].event_type == "decal.placement.create" &&
            events.value()[1U].event_type == "decal.placement.update",
        "only accepted decal placement transitions emit journal events");
}

} // namespace

int main() {
    try {
        projection_is_bounded_and_keeps_source_correspondence();
        stale_identity_and_projection_limits_fail_closed();
        placement_revision_and_serialization_are_fail_closed();
        placement_serialization_is_locale_independent();
        placement_project_transaction_persists_revision_and_journal();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "decal tests passed\n";
    return EXIT_SUCCESS;
}
