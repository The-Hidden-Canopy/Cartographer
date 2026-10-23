#include <carto/biological/authoring.hpp>
#include <carto/biological/proxy_mesh.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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
                .proxy_surface = std::nullopt,
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
                .proxy_surface = std::nullopt,
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
                .proxy_surface = std::nullopt,
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
                .family = std::nullopt,
                .uncertainty_digest = std::nullopt,
                .uncertainty_statement = {},
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
                .family = std::nullopt,
                .uncertainty_digest = std::nullopt,
                .uncertainty_statement = {},
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
                .continuity = RepresentationContinuityId{101U},
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
                .continuity = RepresentationContinuityId{102U},
                .layers = {LayerId{1U}, LayerId{2U}},
            })),
        "gameplay representation enters the family");
    require(static_cast<bool>(document.set_representation_family(std::move(family))),
        "representation family enters the document");
    require(static_cast<bool>(document.validate()), "fixture biological document validates");
    return document;
}

StructureRecord proxy_structure(
    std::uint64_t id,
    StructureKind kind,
    std::optional<StructureId> parent,
    std::string semantic_type,
    std::string_view geometry_label,
    std::optional<kinematic::JointId> joint = std::nullopt) {
    return StructureRecord{
        .id = StructureId{id},
        .kind = kind,
        .parent = parent,
        .semantic_type = std::move(semantic_type),
        .coordinate_frame = "organism.local",
        .kinematic_joint = joint,
        .kinematic_graph_digest = joint.has_value()
            ? std::optional<assets::Sha256Digest>{digest("proxy-kinematic-graph")}
            : std::nullopt,
        .geometry_digest = digest(geometry_label),
        .proxy_surface = EllipsoidProxyProfile{
            .center = {0.0, static_cast<double>(id) * 0.1, 0.0},
            .radii = {0.5 + static_cast<double>(id) * 0.125, 0.75, 0.375},
            .longitude_segments = 12U,
            .latitude_segments = 6U,
        },
    };
}

BiologicalAuthoringDocument biological_proxy_fixture(bool canid) {
    const std::string_view family = canid ? "canid-proxy" : "human-proxy";
    BiologicalAuthoringDocument document;
    document.source = AuthoringSourceIdentity{
        "fixtures/cartographer-biological", canid ? 7002U : 7003U,
        core::Revision{3U}, digest(std::string(family) + ":source-v3")};
    document.organism_family = std::string(family);

    std::vector<StructureRecord> structures;
    if (canid) {
        structures = {
            proxy_structure(1U, StructureKind::organism, std::nullopt,
                "canid.base", "canid-base-geometry"),
            proxy_structure(2U, StructureKind::bone, StructureId{1U},
                "axial.skeleton", "canid-axial-geometry"),
            proxy_structure(3U, StructureKind::appendage, StructureId{1U},
                "forelimb.proxy", "canid-forelimb-geometry"),
            proxy_structure(4U, StructureKind::appendage, StructureId{1U},
                "hindlimb.proxy", "canid-hindlimb-geometry"),
            proxy_structure(5U, StructureKind::appendage, StructureId{1U},
                "muzzle.proxy", "canid-muzzle-geometry"),
            proxy_structure(6U, StructureKind::skin, StructureId{1U},
                "pelt.surface", "canid-pelt-geometry"),
        };
    } else {
        structures = {
            proxy_structure(1U, StructureKind::organism, std::nullopt,
                "human.proxy", "human-base-geometry"),
            proxy_structure(2U, StructureKind::bone, StructureId{1U},
                "axial.skeleton", "human-axial-geometry"),
            proxy_structure(3U, StructureKind::muscle, StructureId{1U},
                "torso.mass", "human-torso-geometry"),
            proxy_structure(4U, StructureKind::appendage, StructureId{1U},
                "upper-limb.proxy", "human-upper-limb-geometry",
                kinematic::JointId{21U}),
            proxy_structure(5U, StructureKind::appendage, StructureId{1U},
                "lower-limb.proxy", "human-lower-limb-geometry",
                kinematic::JointId{22U}),
            proxy_structure(6U, StructureKind::skin, StructureId{1U},
                "body.surface", "human-surface-geometry"),
        };
    }
    for (const auto& structure : structures) {
        require(static_cast<bool>(document.insert_structure(structure)),
            "generic biological proxy structure is admitted");
    }

    require(static_cast<bool>(document.insert_field(field(
                FieldId{1U}, FieldKind::orientation, FieldValueKind::direction,
                FieldRepresentation::geometry))) &&
            static_cast<bool>(document.insert_field(field(
                FieldId{2U}, FieldKind::density, FieldValueKind::scalar,
                FieldRepresentation::statistical_shading))) &&
            static_cast<bool>(document.insert_field(field(
                FieldId{3U}, FieldKind::growth, FieldValueKind::scalar,
                FieldRepresentation::geometry))),
        "proxy morphology fields are source-backed and independently typed");
    require(static_cast<bool>(document.insert_attachment(AttachmentRecord{
                .id = AttachmentId{1U},
                .first_structure = StructureId{2U},
                .second_structure = StructureId{canid ? 3U : 4U},
                .relation = RelationKind::attached_to,
                .first_anchor = "proximal",
                .second_anchor = "root",
                .driver_field = FieldId{1U},
                .evidence_digest = digest(std::string(family) + ":attachment"),
            })),
        "proxy attachment binds stable authored structures and evidence");
    require(static_cast<bool>(document.insert_layer(LayerRecord{
                .id = LayerId{1U},
                .kind = LayerKind::anatomy,
                .owner = StructureId{2U},
                .parent = std::nullopt,
                .order = 10U,
                .semantic_type = "anatomy.source",
                .fields = {FieldId{1U}},
                .family = std::nullopt,
                .uncertainty_digest = std::nullopt,
                .uncertainty_statement = {},
            })) &&
            static_cast<bool>(document.insert_layer(LayerRecord{
                .id = LayerId{2U},
                .kind = canid ? LayerKind::strand : LayerKind::skin,
                .owner = StructureId{6U},
                .parent = std::nullopt,
                .order = 20U,
                .semantic_type = canid ? "covering.strand" : "surface.skin",
                .fields = {FieldId{2U}, FieldId{3U}},
                .family = std::nullopt,
                .uncertainty_digest = std::nullopt,
                .uncertainty_statement = {},
            })),
        "proxy anatomy and covering remain separate authored layers");
    require(static_cast<bool>(document.insert_stage(StageRecord{
                .id = StageId{1U},
                .name = "juvenile",
                .normalized_position = 0.25,
                .field_overrides = {FieldId{1U}, FieldId{3U}},
            })) &&
            static_cast<bool>(document.insert_stage(StageRecord{
                .id = StageId{2U},
                .name = "adult",
                .normalized_position = 0.75,
                .field_overrides = {FieldId{1U}, FieldId{2U}, FieldId{3U}},
            })),
        "proxy morphology stages remain source alternatives, not runtime age state");

    RepresentationFamily family_representations;
    family_representations.set_id(canid ? 42U : 43U);
    for (const auto& [id, kind, label, continuity] : {
             std::tuple{RepresentationId{1U}, RepresentationKind::hero,
                 "hero", 601U},
             std::tuple{RepresentationId{2U}, RepresentationKind::gameplay,
                 "gameplay", 602U}}) {
        require(static_cast<bool>(family_representations.upsert_variant(
                    RepresentationVariant{
                        .id = id,
                        .kind = kind,
                        .artifact_digest = digest(std::string(family) + ":" + label),
                        .error_envelope_digest = digest(
                            std::string(family) + ":" + label + ":error"),
                        .correspondence_digest = digest(
                            std::string(family) + ":" + label + ":correspondence"),
                        .resident_bytes = id.value == 1U ? 4096U : 1024U,
                        .continuity = RepresentationContinuityId{
                            continuity + (canid ? 0U : 10U)},
                        .layers = {LayerId{1U}, LayerId{2U}},
                    })),
            "proxy source can bind distinct derived representation receipts");
    }
    require(static_cast<bool>(document.set_representation_family(
                std::move(family_representations))) &&
            static_cast<bool>(document.validate_for_publication()),
        "biological proxy fixture is admissible as a public source document");
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

void canid_and_human_proxy_sources_round_trip_without_runtime_identity() {
    const std::array<BiologicalAuthoringDocument, 2U> fixtures{
        biological_proxy_fixture(true), biological_proxy_fixture(false)};
    for (const auto& source : fixtures) {
        const auto before_digest = source.canonical_digest();
        require(static_cast<bool>(before_digest),
            "biological proxy source digest computes");
        const std::string encoded = source.serialize();
        const auto reopened = BiologicalAuthoringDocument::deserialize(encoded);
        require(static_cast<bool>(reopened) &&
                reopened.value().organism_family == source.organism_family &&
                reopened.value().structures().size() == 6U &&
                reopened.value().fields().size() == 3U &&
                reopened.value().attachments().size() == 1U &&
                reopened.value().layers().size() == 2U &&
                reopened.value().stages().size() == 2U &&
                reopened.value().representations().has_value() &&
                reopened.value().representations()->variants().size() == 2U,
            "canid and human proxy fixtures preserve independent source records on reopen");
        const auto after_digest = reopened.value().canonical_digest();
        require(after_digest && before_digest.value() == after_digest.value(),
            "canid and human proxy source digests remain stable across reopen");
        require(encoded.find("runtime_handle") == std::string::npos &&
                encoded.find("runtime_entity") == std::string::npos &&
                encoded.find("telemetry") == std::string::npos &&
                encoded.find("health") == std::string::npos &&
                encoded.find("pose") == std::string::npos,
            "biological proxy source stores no runtime identity or gameplay state");

        const auto first_mesh = compile_ellipsoid_proxy(source, StructureId{1U});
        const auto repeated_mesh = compile_ellipsoid_proxy(source, StructureId{1U});
        require(first_mesh && repeated_mesh,
            "explicit source profiles compile to proxy products");
        require(first_mesh.value().mesh.vertex_count() == 62U &&
                first_mesh.value().mesh.face_count() == 72U,
            "proxy tessellation respects the source ring and segment counts");
        require(static_cast<bool>(first_mesh.value().mesh.validate()),
            "generated proxy mesh has valid editable topology");
        const auto topology = first_mesh.value().mesh.topology();
        require(topology && topology.value().boundary_half_edges().empty(),
            "generated proxy mesh is closed without open boundary edges");
        require(first_mesh.value().product_digest == repeated_mesh.value().product_digest,
            "proxy compilation is deterministic");
        require(static_cast<bool>(first_mesh.value().validate_against(source)),
            "proxy product validates against exact source identity and revision");
        require(!compile_ellipsoid_proxy(source, StructureId{999U}),
            "proxy compilation rejects missing structures instead of inventing geometry");
        std::string changed_text = source.serialize();
        const auto radius = changed_text.find("0.625 0.75 0.375");
        require(radius != std::string::npos, "fixture profile has canonical radius text");
        changed_text.replace(radius, std::string("0.625").size(), "0.75");
        const auto changed_profile = BiologicalAuthoringDocument::deserialize(changed_text);
        require(changed_profile && !first_mesh.value().validate_against(changed_profile.value()),
            "proxy product becomes stale when source-authored morphology changes");
        auto altered_mesh = first_mesh.value();
        const auto* north_pole = altered_mesh.mesh.find_vertex(geometry::VertexId{1U});
        require(north_pole != nullptr, "generated mesh exposes its stable first vertex");
        const core::Vec3d altered_north{
            north_pole->position.x, north_pole->position.y + 0.01, north_pole->position.z};
        require(static_cast<bool>(altered_mesh.mesh.set_vertex_position(
                    geometry::VertexId{1U}, altered_north)) &&
                !altered_mesh.validate_against(source),
            "proxy receipt refuses geometry altered after compilation");

        auto next_revision = reopened.value();
        next_revision.source.revision = source.source.revision.next();
        next_revision.source.digest = digest(source.organism_family + ":source-v4");
        require(static_cast<bool>(verify_reimport_continuity(source, next_revision)),
            "proxy reimport advances the source revision without losing structure identity");
        auto same_revision_replacement = next_revision;
        same_revision_replacement.source.revision = source.source.revision;
        require(!verify_reimport_continuity(source, same_revision_replacement),
            "proxy source replacement at the same revision is rejected");
    }
}

void hostile_inputs_and_cross_record_boundaries_fail_closed() {
    auto document = fixture_document();
    require(!BiologicalAuthoringDocument::deserialize(document.serialize() + "junk"),
        "biological document rejects trailing input");

    std::string future = document.serialize();
    const std::string current = "CARTOGRAPHER_BIOLOGICAL_AUTHORING 2";
    future.replace(0U, current.size(), "CARTOGRAPHER_BIOLOGICAL_AUTHORING 99");
    const auto future_result = BiologicalAuthoringDocument::deserialize(future);
    require(!future_result && future_result.error().code == core::ErrorCode::version_mismatch,
        "future biological document versions fail with version mismatch");

    EllipsoidProxyProfile profile;
    profile.longitude_segments = std::numeric_limits<std::uint32_t>::max();
    profile.latitude_segments = std::numeric_limits<std::uint32_t>::max();
    require(!profile.validate(),
        "proxy tessellation rejects hostile segment values before arithmetic can overflow");
    profile.longitude_segments = 128U;
    profile.latitude_segments = 65U;
    require(static_cast<bool>(profile.validate()),
        "proxy tessellation accepts its exact bounded triangle budget");
    profile.latitude_segments = 66U;
    require(!profile.validate(), "proxy tessellation rejects budget overflow");

    BiologicalAuthoringDocument cyclic;
    cyclic.source = document.source;
    cyclic.organism_family = "cycle-fixture";
    require(static_cast<bool>(cyclic.insert_structure(StructureRecord{
                .id = StructureId{1U}, .kind = StructureKind::organism,
                .parent = StructureId{2U}, .semantic_type = "a",
                .coordinate_frame = "local", .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt, .geometry_digest = std::nullopt,
                .proxy_surface = std::nullopt})),
        "first cyclic structure is admitted before aggregate validation");
    require(static_cast<bool>(cyclic.insert_structure(StructureRecord{
                .id = StructureId{2U}, .kind = StructureKind::surface,
                .parent = StructureId{1U}, .semantic_type = "b",
                .coordinate_frame = "local", .kinematic_joint = std::nullopt,
                .kinematic_graph_digest = std::nullopt, .geometry_digest = std::nullopt,
                .proxy_surface = std::nullopt})),
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
                .continuity = RepresentationContinuityId{103U},
                .layers = {LayerId{1U}},
            }),
        "duplicate representation tier is rejected");
}

void typed_layer_families_round_trip_and_reject_incomplete_evidence() {
    const std::vector<std::pair<LayerFamily, LayerKind>> families{
        {LayerFamily::hair, LayerKind::strand},
        {LayerFamily::fur, LayerKind::strand},
        {LayerFamily::feather, LayerKind::feather},
        {LayerFamily::weave, LayerKind::weave},
        {LayerFamily::granular, LayerKind::granular},
        {LayerFamily::connective_tissue, LayerKind::connective_tissue},
        {LayerFamily::muscle, LayerKind::muscle},
        {LayerFamily::skin, LayerKind::skin},
        {LayerFamily::face, LayerKind::face},
        {LayerFamily::injury, LayerKind::injury_overlay},
        {LayerFamily::species_morphology, LayerKind::species_morphology},
        {LayerFamily::biomechanics, LayerKind::biomechanics},
        {LayerFamily::anatomy, LayerKind::anatomy},
        {LayerFamily::soft_tissue, LayerKind::soft_tissue},
        {LayerFamily::adipose, LayerKind::adipose},
        {LayerFamily::organ, LayerKind::organ},
        {LayerFamily::vascular, LayerKind::vascular},
        {LayerFamily::neural, LayerKind::neural},
        {LayerFamily::fluid, LayerKind::fluid},
        {LayerFamily::growth, LayerKind::growth},
    };
    const auto required_fields = [](LayerFamily family) {
        switch (family) {
        case LayerFamily::hair: return std::vector<FieldKind>{FieldKind::orientation,
            FieldKind::density, FieldKind::length, FieldKind::radius};
        case LayerFamily::fur: return std::vector<FieldKind>{FieldKind::orientation,
            FieldKind::density, FieldKind::length, FieldKind::radius, FieldKind::coverage};
        case LayerFamily::feather: return std::vector<FieldKind>{FieldKind::orientation,
            FieldKind::density, FieldKind::length, FieldKind::thickness};
        case LayerFamily::weave: return std::vector<FieldKind>{FieldKind::orientation,
            FieldKind::density, FieldKind::thickness, FieldKind::compaction};
        case LayerFamily::granular: return std::vector<FieldKind>{FieldKind::density,
            FieldKind::radius, FieldKind::compaction};
        case LayerFamily::connective_tissue: return std::vector<FieldKind>{
            FieldKind::orientation, FieldKind::thickness, FieldKind::compliance};
        case LayerFamily::muscle: return std::vector<FieldKind>{FieldKind::orientation,
            FieldKind::activation, FieldKind::mass};
        case LayerFamily::skin: return std::vector<FieldKind>{FieldKind::thickness,
            FieldKind::pigmentation, FieldKind::damage};
        case LayerFamily::face: return std::vector<FieldKind>{FieldKind::thickness,
            FieldKind::pigmentation, FieldKind::variation};
        case LayerFamily::injury: return std::vector<FieldKind>{FieldKind::damage,
            FieldKind::deformation};
        case LayerFamily::species_morphology: return std::vector<FieldKind>{
            FieldKind::growth, FieldKind::mass, FieldKind::variation};
        case LayerFamily::biomechanics: return std::vector<FieldKind>{FieldKind::compliance,
            FieldKind::activation, FieldKind::mass, FieldKind::inertia, FieldKind::contact};
        case LayerFamily::anatomy: return std::vector<FieldKind>{FieldKind::mass,
            FieldKind::variation};
        case LayerFamily::soft_tissue: return std::vector<FieldKind>{FieldKind::thickness,
            FieldKind::compliance};
        case LayerFamily::adipose: return std::vector<FieldKind>{FieldKind::thickness,
            FieldKind::mass, FieldKind::moisture};
        case LayerFamily::organ: return std::vector<FieldKind>{FieldKind::mass,
            FieldKind::variation, FieldKind::compliance};
        case LayerFamily::vascular: return std::vector<FieldKind>{FieldKind::radius,
            FieldKind::thickness, FieldKind::compliance};
        case LayerFamily::neural: return std::vector<FieldKind>{FieldKind::activation,
            FieldKind::variation};
        case LayerFamily::fluid: return std::vector<FieldKind>{FieldKind::density,
            FieldKind::mass, FieldKind::moisture};
        case LayerFamily::growth: return std::vector<FieldKind>{FieldKind::growth,
            FieldKind::variation, FieldKind::mass};
        }
        return std::vector<FieldKind>{};
    };

    auto document = fixture_document();
    std::map<FieldKind, FieldId> field_ids{
        {FieldKind::orientation, FieldId{1U}},
        {FieldKind::density, FieldId{2U}},
        {FieldKind::activation, FieldId{3U}},
    };
    std::uint64_t next_field_id = 4U;
    const std::array all_field_kinds{
        FieldKind::orientation, FieldKind::density, FieldKind::radius, FieldKind::length,
        FieldKind::thickness, FieldKind::coverage, FieldKind::variation,
        FieldKind::pigmentation, FieldKind::moisture, FieldKind::compaction,
        FieldKind::deformation, FieldKind::compliance, FieldKind::activation,
        FieldKind::damage, FieldKind::growth, FieldKind::mass, FieldKind::inertia,
        FieldKind::contact};
    for (const FieldKind kind : all_field_kinds) {
        if (field_ids.contains(kind)) continue;
        const FieldValueKind value_kind = kind == FieldKind::deformation
            ? FieldValueKind::vector : FieldValueKind::scalar;
        const FieldId id{next_field_id++};
        require(static_cast<bool>(document.insert_field(field(
                    id, kind, value_kind, FieldRepresentation::geometry))),
            "typed family source field is admitted");
        field_ids.emplace(kind, id);
    }

    LayerId next_layer_id{3U};
    std::vector<LayerRecord> authored_layers;
    for (const auto& [family, kind] : families) {
        LayerRecord layer{
            .id = next_layer_id,
            .kind = kind,
            .owner = StructureId{1U},
            .parent = std::nullopt,
            .order = static_cast<std::uint32_t>(next_layer_id.value),
            .semantic_type = std::string(layer_family_name(family)),
            .fields = {},
            .family = family,
            .uncertainty_digest = digest("uncertainty-" +
                std::string(layer_family_name(family))),
            .uncertainty_statement = "Source estimate; reviewer confirmation required.",
        };
        for (const FieldKind required_kind : required_fields(family)) {
            layer.fields.push_back(field_ids.at(required_kind));
        }
        std::sort(layer.fields.begin(), layer.fields.end());
        require(static_cast<bool>(document.insert_layer(layer)),
            "registered typed family layer is admitted");
        authored_layers.push_back(layer);
        next_layer_id = LayerId{next_layer_id.value + 1U};
    }
    require(static_cast<bool>(document.validate()),
        "all registered families satisfy their typed source contracts");
    const auto encoded = document.serialize();
    require(encoded.starts_with("CARTOGRAPHER_BIOLOGICAL_AUTHORING 3\n"),
        "typed family records use the uncertainty-aware authoring schema");
    const auto decoded = BiologicalAuthoringDocument::deserialize(encoded);
    require(static_cast<bool>(decoded),
        "document with typed family layers survives canonical save/reopen");
    const auto generic = decoded.value().layers().find(LayerId{1U});
    require(generic != decoded.value().layers().end() &&
            generic->second.kind == LayerKind::microstructure &&
            !generic->second.family.has_value() &&
            !generic->second.uncertainty_digest.has_value() &&
            generic->second.uncertainty_statement.empty(),
        "generic layers remain generic beside typed family layers");
    require(decoded.value().serialize() == encoded,
        "mixed schema-3 records survive a canonical byte round trip");
    for (std::size_t index = 0U; index < families.size(); ++index) {
        const auto [family, kind] = families[index];
        const auto found = decoded.value().layers().find(authored_layers[index].id);
        require(found != decoded.value().layers().end(),
            "all family layer identities survive canonical save/reopen");
        const auto& layer = found->second;
        const auto parsed_name = layer_family_from_name(layer_family_name(family));
        require(layer.family == family && layer.kind == kind && parsed_name &&
                parsed_name.value() == family && layer.uncertainty_digest.has_value() &&
                !layer.uncertainty_statement.empty(),
            "family identity, layer kind, and explicit uncertainty round-trip");
    }

    auto wrong_kind = authored_layers.front();
    wrong_kind.kind = LayerKind::microstructure;
    require(!wrong_kind.validate(), "wrong layer tier for a typed family is refused");
    auto missing_uncertainty = authored_layers.front();
    missing_uncertainty.uncertainty_digest.reset();
    missing_uncertainty.uncertainty_statement.clear();
    require(!missing_uncertainty.validate(), "family layer without uncertainty is refused");

    auto missing_field_document = fixture_document();
    LayerRecord incomplete{
        .id = LayerId{3U}, .kind = LayerKind::strand, .owner = StructureId{1U},
        .parent = std::nullopt, .order = 0U,
        .semantic_type = "hair", .fields = {FieldId{1U}},
        .family = LayerFamily::hair, .uncertainty_digest = digest("hair-uncertainty"),
        .uncertainty_statement = "Estimate; review required.",
    };
    require(static_cast<bool>(missing_field_document.insert_layer(incomplete)) &&
            !missing_field_document.validate(),
        "family missing required source field kinds is rejected at document validation");
}

void schema_one_source_migrates_without_invented_geometry() {
    const auto source = fixture_document();
    std::istringstream input(source.serialize());
    std::ostringstream legacy;
    std::string line;
    bool structure_records = false;
    legacy << "CARTOGRAPHER_BIOLOGICAL_AUTHORING 1\n";
    static_cast<void>(std::getline(input, line));
    while (std::getline(input, line)) {
        if (line.starts_with("STRUCTURES ")) structure_records = true;
        if (structure_records && line.starts_with("FIELDS ")) structure_records = false;
        if (structure_records && line.starts_with("STRUCTURE ")) {
            const auto marker = line.rfind(" 0");
            require(marker != std::string::npos && marker + 2U == line.size(),
                "schema-2 structure has a terminal optional-profile marker");
            line.erase(marker);
        }
        legacy << line << '\n';
    }
    const auto reopened = BiologicalAuthoringDocument::deserialize(legacy.str());
    require(reopened && reopened.value().structures().size() == 3U &&
            !reopened.value().structures().at(StructureId{1U}).proxy_surface &&
            reopened.value().serialize().starts_with(
                "CARTOGRAPHER_BIOLOGICAL_AUTHORING 2\n") &&
            !compile_ellipsoid_proxy(reopened.value(), StructureId{1U}),
        "schema-1 migration preserves metadata and does not fabricate proxy geometry");
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
    canid_and_human_proxy_sources_round_trip_without_runtime_identity();
    schema_one_source_migrates_without_invented_geometry();
    hostile_inputs_and_cross_record_boundaries_fail_closed();
    typed_layer_families_round_trip_and_reject_incomplete_evidence();
    publication_and_reimport_gates_reject_stale_state();
    std::cout << "biological authoring tests passed\n";
    return EXIT_SUCCESS;
}
