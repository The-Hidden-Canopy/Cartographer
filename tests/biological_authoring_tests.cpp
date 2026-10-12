#include <carto/biological/authoring.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace carto;
using namespace carto::biological;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

FieldRecord field(
    FieldId id,
    FieldKind kind,
    FieldValueKind value_kind,
    FieldRepresentation representation,
    std::optional<FieldId> coverage = std::nullopt) {
    return FieldRecord{
        .id = id,
        .kind = kind,
        .value_kind = value_kind,
        .representation = representation,
        .domain = "surface",
        .coordinate_frame = "organism.local",
        .units = "unit",
        .sample_count = 32U,
        .stable_seed = 100U + id.value,
        .coverage_field = coverage,
        .data_digest = digest("field-data-" + std::to_string(id.value)),
        .minimum = 0.0,
        .maximum = 1.0,
    };
}

BiologicalAuthoringDocument fixture_document() {
    BiologicalAuthoringDocument document;
    document.source = AuthoringSourceIdentity{
        "fixtures/cartographer-biological", 7001U, core::Revision{4U},
        digest("biological-source-v4")};
    document.organism_family = "shared-organism-fixture";

    require(static_cast<bool>(document.insert_structure(StructureRecord{
                .id = StructureId{1U},
                .kind = StructureKind::organism,
                .parent = std::nullopt,
                .semantic_type = "organism",
                .coordinate_frame = "organism.local",
                .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt,
                .geometry_digest = digest("organism-geometry"),
            })),
        "organism structure enters the document");
    require(static_cast<bool>(document.insert_structure(StructureRecord{
                .id = StructureId{2U},
                .kind = StructureKind::skin,
                .parent = StructureId{1U},
                .semantic_type = "skin.surface",
                .coordinate_frame = "organism.local",
                .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt,
                .geometry_digest = digest("skin-geometry"),
            })),
        "skin structure enters the document");
    require(static_cast<bool>(document.insert_structure(StructureRecord{
                .id = StructureId{3U},
                .kind = StructureKind::muscle,
                .parent = StructureId{1U},
                .semantic_type = "muscle.flexor",
                .coordinate_frame = "organism.local",
                .kinematic_joint = kinematic::JointId{11U},
                .kinematic_graph_digest = digest("kinematic-graph-v4"),
                .geometry_digest = digest("muscle-geometry"),
            })),
        "muscle structure with kinematic correspondence enters");

    require(static_cast<bool>(document.insert_field(field(
                FieldId{1U}, FieldKind::orientation, FieldValueKind::direction,
                FieldRepresentation::geometry))),
        "orientation field enters the document");
    require(static_cast<bool>(document.insert_field(field(
                FieldId{2U}, FieldKind::density, FieldValueKind::scalar,
                FieldRepresentation::statistical_shading, FieldId{1U}))),
        "density field with coverage enters the document");
    require(static_cast<bool>(document.insert_field(field(
                FieldId{3U}, FieldKind::activation, FieldValueKind::scalar,
                FieldRepresentation::constraint))),
        "activation field enters the document");

    require(static_cast<bool>(document.insert_attachment(AttachmentRecord{
                .id = AttachmentId{1U},
                .first_structure = StructureId{3U},
                .second_structure = StructureId{2U},
                .relation = RelationKind::origin_insertion,
                .first_anchor = "origin",
                .second_anchor = "dermis",
                .driver_field = FieldId{3U},
                .evidence_digest = digest("attachment-evidence"),
            })),
        "attachment enters the document");

    require(static_cast<bool>(document.insert_layer(LayerRecord{
                .id = LayerId{1U},
                .kind = LayerKind::microstructure,
                .owner = StructureId{2U},
                .parent = std::nullopt,
                .order = 10U,
                .semantic_type = "skin.microstructure",
                .fields = {FieldId{1U}, FieldId{2U}},
            })),
        "microstructure layer enters the document");
    require(static_cast<bool>(document.insert_layer(LayerRecord{
                .id = LayerId{2U},
                .kind = LayerKind::muscle,
                .owner = StructureId{3U},
                .parent = std::nullopt,
                .order = 20U,
                .semantic_type = "muscle.activation",
                .fields = {FieldId{3U}},
            })),
        "muscle layer enters the document");
    require(static_cast<bool>(document.insert_stage(StageRecord{
                .id = StageId{1U},
                .name = "adult",
                .normalized_position = 0.5,
                .field_overrides = {FieldId{1U}, FieldId{2U}},
            })),
        "adult stage enters the document");

    RepresentationFamily family;
    family.set_id(41U);
    require(static_cast<bool>(family.upsert_variant(RepresentationVariant{
                .id = RepresentationId{1U},
                .kind = RepresentationKind::hero,
                .artifact_digest = digest("hero-artifact"),
                .error_envelope_digest = digest("hero-error"),
                .correspondence_digest = digest("hero-correspondence"),
                .resident_bytes = 1024U,
                .continuity = representation::RepresentationContinuityId{101U},
                .layers = {LayerId{1U}, LayerId{2U}},
            })),
        "hero representation enters the family");
    require(static_cast<bool>(family.upsert_variant(RepresentationVariant{
                .id = RepresentationId{2U},
                .kind = RepresentationKind::gameplay,
                .artifact_digest = digest("gameplay-artifact"),
                .error_envelope_digest = digest("gameplay-error"),
                .correspondence_digest = digest("gameplay-correspondence"),
                .resident_bytes = 512U,
                .continuity = representation::RepresentationContinuityId{102U},
                .layers = {LayerId{1U}, LayerId{2U}},
            })),
        "gameplay representation enters the family");
    require(static_cast<bool>(document.set_representation_family(std::move(family))),
        "representation family enters the document");
    require(static_cast<bool>(document.validate()), "fixture biological document validates");
    return document;
}

void document_round_trip_binds_every_layer() {
    const auto document = fixture_document();
    const auto first_digest = document.canonical_digest();
    require(static_cast<bool>(first_digest), "biological document digest computes");
    const auto encoded = document.serialize();
    const auto reparsed = BiologicalAuthoringDocument::deserialize(encoded);
    require(static_cast<bool>(reparsed), "biological document round-trips");
    const auto second_digest = reparsed.value().canonical_digest();
    require(static_cast<bool>(second_digest), "reparsed biological digest computes");
    require(first_digest.value() == second_digest.value(),
        "biological document digest is stable across serialization");
    require(reparsed.value().structures().size() == 3U &&
                reparsed.value().fields().size() == 3U &&
                reparsed.value().layers().size() == 2U &&
                reparsed.value().representations()->variants().size() == 2U,
        "round trip preserves structures, fields, layers, and representations");
    require(encoded.find("runtime_handle") == std::string::npos &&
                encoded.find("telemetry") == std::string::npos &&
                encoded.find("pose") == std::string::npos &&
                encoded.find("health") == std::string::npos,
        "public biological source serialization contains no runtime or game state");
}

void hostile_inputs_and_cross_record_boundaries_fail_closed() {
    auto document = fixture_document();
    require(!BiologicalAuthoringDocument::deserialize(document.serialize() + "junk"),
        "biological document rejects trailing input");

    std::string future = document.serialize();
    const std::string current = "CARTOGRAPHER_BIOLOGICAL_AUTHORING 1";
    future.replace(0U, current.size(), "CARTOGRAPHER_BIOLOGICAL_AUTHORING 99");
    const auto future_result = BiologicalAuthoringDocument::deserialize(future);
    require(!future_result && future_result.error().code == core::ErrorCode::version_mismatch,
        "future biological document versions fail with version mismatch");

    BiologicalAuthoringDocument cyclic;
    cyclic.source = document.source;
    cyclic.organism_family = "cycle-fixture";
    require(static_cast<bool>(cyclic.insert_structure(StructureRecord{
                .id = StructureId{1U}, .kind = StructureKind::organism,
                .parent = StructureId{2U}, .semantic_type = "a",
                .coordinate_frame = "local", .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt, .geometry_digest = std::nullopt})),
        "first cyclic structure is admitted before aggregate validation");
    require(static_cast<bool>(cyclic.insert_structure(StructureRecord{
                .id = StructureId{2U}, .kind = StructureKind::surface,
                .parent = StructureId{1U}, .semantic_type = "b",
                .coordinate_frame = "local", .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt, .geometry_digest = std::nullopt})),
        "second cyclic structure is admitted before aggregate validation");
    require(!cyclic.validate(), "cyclic biological structure hierarchy is rejected");

    BiologicalAuthoringDocument coverage_cycle;
    coverage_cycle.source = document.source;
    coverage_cycle.organism_family = "coverage-cycle-fixture";
    auto coverage_one = field(
        FieldId{11U}, FieldKind::coverage, FieldValueKind::scalar,
        FieldRepresentation::geometry, FieldId{12U});
    auto coverage_two = field(
        FieldId{12U}, FieldKind::coverage, FieldValueKind::scalar,
        FieldRepresentation::geometry, FieldId{11U});
    require(static_cast<bool>(coverage_cycle.insert_field(std::move(coverage_one))) &&
                static_cast<bool>(coverage_cycle.insert_field(std::move(coverage_two))),
        "coverage-cycle fields enter before aggregate validation");
    require(!coverage_cycle.validate(), "cyclic field coverage is rejected");

    auto invalid_field = field(
        FieldId{99U}, FieldKind::density, FieldValueKind::scalar,
        FieldRepresentation::statistical_shading);
    invalid_field.minimum = 2.0;
    invalid_field.maximum = 1.0;
    require(!invalid_field.validate(), "reversed field bounds are rejected");

    auto family = *document.representations();
    require(!family.upsert_variant(RepresentationVariant{
                .id = RepresentationId{3U},
                .kind = RepresentationKind::hero,
                .artifact_digest = digest("duplicate-kind"),
                .error_envelope_digest = digest("duplicate-error"),
                .correspondence_digest = digest("duplicate-correspondence"),
                .resident_bytes = 1U,
                .continuity = representation::RepresentationContinuityId{103U},
                .layers = {LayerId{1U}},
            }),
        "duplicate representation tier is rejected");
}

void publication_and_reimport_gates_reject_stale_state() {
    auto document = fixture_document();
    const auto authoring_digest = document.canonical_digest();
    require(static_cast<bool>(authoring_digest), "publication fixture digest computes");
    PreparedPublication publication{
        .source = document.source,
        .authoring_digest = authoring_digest.value(),
        .prepared_digest = digest("prepared-product-v4"),
        .target_profile = "gameplay-render",
        .compiler = "cartographer-public-authoring/1",
        .representations = {RepresentationId{1U}, RepresentationId{2U}},
        .receipt_digest = {},
    };
    require(static_cast<bool>(publication.refresh_receipt_digest()),
        "publication receipt binds its content");
    require(static_cast<bool>(validate_prepared_publication(document, publication)),
        "current publication is admitted");
    const auto publication_round_trip = PreparedPublication::deserialize(publication.serialize());
    require(static_cast<bool>(publication_round_trip), "publication receipt round-trips");

    auto stale_publication = publication;
    stale_publication.source.revision = core::Revision{3U};
    require(static_cast<bool>(stale_publication.refresh_receipt_digest()),
        "stale fixture receipt can be re-signed as a separate value");
    require(!validate_prepared_publication(document, stale_publication),
        "publication from an older source revision is rejected");

    auto stale_document = document;
    stale_document.source.revision = document.source.revision;
    stale_document.source.digest = digest("same-revision-different-source");
    require(!validate_prepared_publication(stale_document, publication),
        "publication is rejected after same-revision source replacement");

    auto reimport = document;
    reimport.source.revision = core::Revision{5U};
    auto family = *reimport.representations();
    auto replacement = *family.find(RepresentationId{2U});
    replacement.artifact_digest = digest("gameplay-artifact-v5");
    require(static_cast<bool>(family.upsert_variant(replacement)),
        "representation replacement preserves continuity");
    require(static_cast<bool>(reimport.set_representation_family(std::move(family))),
        "reimported representation family validates");
    require(static_cast<bool>(verify_reimport_continuity(document, reimport)),
        "revision-advanced reimport preserves authored identity and continuity");

    auto same_revision_change = reimport;
    same_revision_change.source.revision = document.source.revision;
    same_revision_change.source.digest = digest("same-revision-change");
    require(!verify_reimport_continuity(document, same_revision_change),
        "same-revision source digest change is stale");
}

} // namespace

int main() {
    document_round_trip_binds_every_layer();
    hostile_inputs_and_cross_record_boundaries_fail_closed();
    publication_and_reimport_gates_reject_stale_state();
    std::cout << "biological authoring tests passed\n";
    return EXIT_SUCCESS;
}
