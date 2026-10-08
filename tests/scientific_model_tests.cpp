#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include <carto/scientific/model.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-scientific-tests-" + std::to_string(stamp));
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

carto::scientific::Provenance fixture_provenance() {
    return carto::scientific::Provenance{
        "fixture://scientific-model",
        "2026.10",
        "CC0-1.0",
        "cartographer-test-provider",
        "2026-10-03T00:00:00Z",
        std::nullopt,
    };
}

carto::scientific::ScientificModel make_model() {
    using namespace carto::scientific;
    ScientificModel model;
    const auto provenance = fixture_provenance();
    REQUIRE(model.insert_region(Region{
        RegionId{1U}, "organ", GeometryBinding{"mesh", "object:1"}, std::nullopt,
        "", {"anatomy", "reference"}, provenance}));
    REQUIRE(model.insert_region(Region{
        RegionId{2U}, "tissue", GeometryBinding{}, RegionId{1U}, "muscle",
        {"tissue"}, provenance}));
    REQUIRE(model.insert_boundary(Boundary{
        BoundaryId{1U}, "surface", GeometryBinding{"mesh", "face:12"}, RegionId{2U},
        provenance}));
    REQUIRE(model.insert_interface(Interface{
        InterfaceId{1U}, "tissue-contact", RegionId{1U}, RegionId{2U}, provenance}));
    REQUIRE(model.insert_source(Source{
        SourceId{1U}, "morphogen", RegionId{2U}, "mol/m3", 0.4, provenance}));
    REQUIRE(model.insert_load(Load{
        LoadId{1U}, "growth-pressure", RegionId{2U}, "Pa", 12.0, provenance}));
    REQUIRE(model.insert_parameter(Parameter{
        ParameterId{1U}, "growth-rate", "1/s", 0.25, 0.0, 1.0, provenance}));
    REQUIRE(model.insert_field(Field{
        FieldId{1U}, FieldKind::scalar, Association::continuous, "mol/m3", "world",
        "stage:1", StudyId{1U}, RegionId{2U}, "reference-provider", std::nullopt,
        provenance}));
    REQUIRE(model.insert_probe(Probe{
        ProbeId{1U}, "tissue-center", RegionId{2U}, FieldId{1U}, "landmark:center",
        provenance}));
    REQUIRE(model.insert_study(Study{
        StudyId{1U}, carto::core::Revision{4U}, {RegionId{1U}, RegionId{2U}},
        {BoundaryId{1U}}, {InterfaceId{1U}}, {SourceId{1U}}, {LoadId{1U}},
        {ParameterId{1U}}, "deterministic-reference", "reference-provider",
        {FieldId{1U}}, "acceptance=fixture", std::nullopt, provenance}));
    REQUIRE(model.insert_result_set(ResultSet{
        ResultSetId{1U}, StudyId{1U}, {FieldId{1U}}, {ProbeId{1U}},
        "reference-provider", "1.0", std::nullopt, std::nullopt,
        "residual=0;iterations=1", provenance}));
    MorphologyModel morphology;
    morphology.organism_identity = "fixture-organism";
    morphology.body_plan = "bilateral-segmented";
    morphology.axes = {"anterior-posterior", "dorsal-ventral"};
    morphology.symmetry = "bilateral";
    REQUIRE(morphology.insert_structure(MorphologyStructure{
        StructureId{1U}, "organ", std::nullopt, "midline", "stage:0",
        GeometryBinding{"mesh", "object:1"}, "organ-local", {}, "measured", provenance}));
    REQUIRE(morphology.insert_structure(MorphologyStructure{
        StructureId{2U}, "tissue", StructureId{1U}, "left", "stage:1",
        GeometryBinding{}, "organ-local", {LandmarkId{1U}}, "estimated", provenance}));
    REQUIRE(morphology.insert_attachment(MorphologyAttachment{
        AttachmentId{1U}, StructureId{1U}, StructureId{2U}, "embedded", provenance}));
    REQUIRE(morphology.insert_landmark(MorphologyLandmark{
        LandmarkId{1U}, "tissue-origin", StructureId{2U}, {1.0, 2.0, 3.0}, "organ-local",
        provenance}));
    REQUIRE(morphology.insert_growth_domain(MorphologyGrowthDomain{
        GrowthDomainId{1U}, StructureId{2U}, FieldId{1U}, {1.0, 0.0, 0.0}, 0.25,
        {1.0, 0.5, 0.25}, 3.0, "stage:1", "stage:2", provenance}));
    REQUIRE(model.set_morphology(std::move(morphology)));

    FunctionalGenome genome;
    genome.schema = "cartographer.functional-genome.v1";
    genome.developmental_program_reference = "program://fixture-development";
    genome.lineage_reference = "lineage://fixture/organism/0";
    const auto genome_digest = carto::assets::Sha256Digest::from_hex(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(genome_digest);
    genome.source_digest = genome_digest.value();
    REQUIRE(genome.insert_gene(FunctionalGene{
        GeneId{1U}, "growth-rate", "developmental", "continuous", 0.25, "1/s",
        0.0, 1.0, "bounded-uniform", "inherited", provenance}));
    REQUIRE(genome.insert_gene(FunctionalGene{
        GeneId{2U}, "sensor-count", "neural", "integer-like", 4.0, "count",
        0.0, 16.0, "bounded-integer", "inherited", provenance}));
    REQUIRE(model.set_functional_genome(std::move(genome)));
    return model;
}

void scientific_model_round_trips_deterministically() {
    const auto model = make_model();
    REQUIRE(model.validate());
    const auto first = model.serialize();
    REQUIRE(first == model.serialize());
    const auto restored = carto::scientific::ScientificModel::deserialize(first);
    REQUIRE(restored);
    REQUIRE(restored.value().regions().size() == 2U);
    REQUIRE(restored.value().fields().at(carto::scientific::FieldId{1U}).source_study ==
            carto::scientific::StudyId{1U});
    REQUIRE(restored.value().result_sets().at(carto::scientific::ResultSetId{1U}).probes.size() == 1U);
    REQUIRE(restored.value().morphology().has_value());
    REQUIRE(restored.value().morphology()->structures().size() == 2U);
    REQUIRE(restored.value().morphology()->landmarks().at(carto::scientific::LandmarkId{1U}).position.z == 3.0);
    REQUIRE(restored.value().morphology()->growth_domains().size() == 1U);
    REQUIRE(restored.value().morphology()->growth_domains().at(carto::scientific::GrowthDomainId{1U}).driving_field ==
            carto::scientific::FieldId{1U});
    REQUIRE(restored.value().functional_genome().has_value());
    REQUIRE(restored.value().functional_genome()->genes().size() == 2U);
    REQUIRE(restored.value().functional_genome()->lineage_reference ==
            "lineage://fixture/organism/0");
    REQUIRE(restored.value().functional_genome()->source_digest.has_value());
    REQUIRE(restored.value().functional_genome()->source_digest->hex() ==
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    carto::scientific::FunctionalGenome child;
    child.schema = "cartographer.functional-genome.v1";
    child.developmental_program_reference = "program://fixture-development";
    REQUIRE(child.insert_gene(carto::scientific::FunctionalGene{
        carto::scientific::GeneId{1U}, "growth-rate", "developmental", "continuous", 0.5, "1/s",
        0.0, 1.0, "bounded-uniform", "inherited", fixture_provenance()}));
    REQUIRE(child.insert_gene(carto::scientific::FunctionalGene{
        carto::scientific::GeneId{3U}, "branching", "effector", "continuous", 1.0, "ratio",
        0.0, 2.0, "bounded-uniform", "recombined", fixture_provenance()}));
    const auto diff = child.diff_against(*restored.value().functional_genome());
    REQUIRE(diff.added == std::vector<carto::scientific::GeneId>{carto::scientific::GeneId{3U}});
    REQUIRE(diff.removed == std::vector<carto::scientific::GeneId>{carto::scientific::GeneId{2U}});
    REQUIRE(diff.changed == std::vector<carto::scientific::GeneId>{carto::scientific::GeneId{1U}});
    REQUIRE(diff.added_modules == std::vector<std::string>{"effector"});
    REQUIRE(diff.removed_modules == std::vector<std::string>{"neural"});
    REQUIRE(diff.changed_modules == std::vector<std::string>{"developmental"});
    REQUIRE(diff.changed_neural_values.empty());
    REQUIRE(diff.developmental_program_changed == false);
    REQUIRE(diff.lineage_changed);
    REQUIRE(diff.source_identity_changed);

    carto::scientific::FunctionalGenome neural_parent;
    neural_parent.schema = "cartographer.functional-genome.v1";
    REQUIRE(neural_parent.insert_gene(carto::scientific::FunctionalGene{
        carto::scientific::GeneId{1U}, "neural-bias", "neural", "continuous", 0.1, "ratio",
        0.0, 1.0, "bounded-uniform", "inherited", fixture_provenance()}));
    carto::scientific::FunctionalGenome neural_child;
    neural_child.schema = "cartographer.functional-genome.v1";
    REQUIRE(neural_child.insert_gene(carto::scientific::FunctionalGene{
        carto::scientific::GeneId{1U}, "neural-bias", "neural", "continuous", 0.2, "ratio",
        0.0, 1.0, "bounded-uniform", "inherited", fixture_provenance()}));
    const auto neural_diff = neural_child.diff_against(neural_parent);
    REQUIRE(neural_diff.changed_neural_values ==
            std::vector<carto::scientific::GeneId>{carto::scientific::GeneId{1U}});
    REQUIRE(!neural_diff.developmental_program_changed);
    REQUIRE(!neural_diff.lineage_changed);
    REQUIRE(!neural_diff.source_identity_changed);
    std::string legacy_scientific = first;
    const auto morphology_start = legacy_scientific.find("MORPHOLOGY ");
    const auto studies_start = legacy_scientific.find("STUDIES ");
    REQUIRE(morphology_start != std::string::npos && studies_start != std::string::npos);
    legacy_scientific.erase(morphology_start, studies_start - morphology_start);
    const auto scientific_header = legacy_scientific.find("CARTOGRAPHER_SCIENTIFIC_MODEL 12");
    REQUIRE(scientific_header != std::string::npos);
    legacy_scientific.replace(
        scientific_header, std::string("CARTOGRAPHER_SCIENTIFIC_MODEL 12").size(),
        "CARTOGRAPHER_SCIENTIFIC_MODEL 1");
    const auto legacy_scientific_model =
        carto::scientific::ScientificModel::deserialize(legacy_scientific);
    REQUIRE(legacy_scientific_model);
    REQUIRE(!legacy_scientific_model.value().morphology().has_value());
    REQUIRE(!legacy_scientific_model.value().functional_genome().has_value());
    REQUIRE(restored.value().serialize() == first);
}

void artificial_organism_authoring_chain_round_trips_with_previews() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();

    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://fixture-development";
    program.description = "artificial organism development acceptance chain";
    const auto program_digest = carto::assets::Sha256Digest::from_hex(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    const auto result_digest_a = carto::assets::Sha256Digest::from_hex(
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    const auto result_digest_b = carto::assets::Sha256Digest::from_hex(
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    const auto genome_digest = carto::assets::Sha256Digest::from_hex(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(program_digest && result_digest_a && result_digest_b && genome_digest);
    program.source_digest = program_digest.value();
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "seed", "always", provenance}));
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{2U}, 1U, "grown", "always", provenance}));
    REQUIRE(program.insert_instruction(DevelopmentInstruction{
        InstructionId{1U}, StageId{1U}, "differentiate", "morphogenesis", "always",
        {ParameterId{1U}}, {StructureId{2U}}, {}, provenance}));
    REQUIRE(program.insert_instruction(DevelopmentInstruction{
        InstructionId{2U}, StageId{2U}, "grow", "morphogenesis", "always",
        {ParameterId{1U}}, {}, {StructureId{2U}}, provenance}));
    REQUIRE(model.set_developmental_program(std::move(program)));

    DevelopmentalTrace trace;
    trace.program_reference = "program://fixture-development";
    trace.genome_reference = "program://fixture-development";
    trace.program_digest = program_digest.value();
    trace.genome_digest = genome_digest.value();
    REQUIRE(trace.insert_entry(DevelopmentTraceEntry{
        TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{2U}}, {},
        "provider receipt: differentiation admitted", provenance, std::nullopt, {0.25},
        std::uint64_t{101U}, result_digest_a.value()}));
    REQUIRE(trace.insert_entry(DevelopmentTraceEntry{
        TraceId{2U}, StageId{2U}, InstructionId{2U}, {}, {StructureId{2U}},
        "provider receipt: growth admitted", provenance, std::nullopt, {0.75},
        std::uint64_t{102U}, result_digest_b.value()}));
    REQUIRE(model.set_developmental_trace(std::move(trace)));

    PhenotypeCapabilityModel capabilities;
    capabilities.schema = "cartographer.phenotype-capabilities.v1";
    capabilities.compilation_reference = "provider://menagerie/phenotype-preview";
    capabilities.status = "provider_receipt";
    capabilities.provenance = provenance;
    REQUIRE(capabilities.insert_capability(PhenotypeCapability{
        CapabilityId{1U}, "growth capacity", "structural", "present", 0.75, "normalized",
        {StructureId{2U}}, {GeneId{1U}}, {InstructionId{1U}, InstructionId{2U}},
        {TraceId{1U}, TraceId{2U}},
        "gene 1 -> development instructions -> structure 2", provenance}));
    REQUIRE(model.set_phenotype_capabilities(std::move(capabilities)));

    GeneRegulatoryNetwork network;
    network.schema = "cartographer.gene-regulatory.v1";
    network.reference = "provider://fixture-regulatory-network";
    network.status = "provider_receipt";
    network.provenance = provenance;
    REQUIRE(network.insert_element(RegulatoryElement{
        RegulatoryElementId{1U}, GeneId{1U}, "growth enhancer", "enhancer",
        "source://fixture/enhancer-1", provenance}));
    REQUIRE(network.insert_edge(RegulatoryEdge{
        RegulatoryEdgeId{1U}, GeneId{1U}, GeneId{2U}, RegulatoryElementId{1U},
        "activation", "growth-rate > 0", StageId{1U}, provenance}));
    REQUIRE(network.insert_expression(SpatialExpression{
        ExpressionId{1U}, GeneId{2U}, StructureId{2U}, std::nullopt, std::nullopt,
        StageId{2U}, "structure_scalar", 0.75, "", "ratio", provenance}));
    REQUIRE(model.set_gene_regulatory_network(std::move(network)));

    FieldVisualizationModel visualizations;
    visualizations.schema = "cartographer.field-visualizations.v1";
    REQUIRE(visualizations.insert_visualization(FieldVisualization{
        VisualizationId{1U}, FieldId{1U}, RegionId{2U}, "scalar", "viridis", 0.0, 1.0,
        "stage:2", true, provenance}));
    REQUIRE(model.set_field_visualizations(std::move(visualizations)));
    REQUIRE(model.validate());

    const auto timeline = model.build_developmental_timeline();
    REQUIRE(timeline);
    REQUIRE(timeline.value().size() == 2U);
    REQUIRE(timeline.value().at(0U).planned_creates == std::vector<StructureId>{StructureId{2U}});
    REQUIRE(timeline.value().at(1U).observed_modifies == std::vector<StructureId>{StructureId{2U}});
    const auto impact = model.build_developmental_instruction_impact(InstructionId{2U});
    REQUIRE(impact);
    REQUIRE(impact.value().trace_entries == std::vector<TraceId>{TraceId{2U}});

    const auto capability_trace = model.phenotype_capabilities()->trace(CapabilityId{1U});
    REQUIRE(capability_trace);
    REQUIRE(capability_trace.value().contributing_instructions.size() == 2U);
    const auto reverse_trace = model.phenotype_capabilities()->capabilities_for_structure(
        StructureId{2U});
    REQUIRE(reverse_trace);
    REQUIRE(reverse_trace.value() == std::vector<CapabilityId>{CapabilityId{1U}});
    const auto field_preview = model.field_visualizations()->visualizations().at(
        VisualizationId{1U}).preview_samples(std::vector<double>{0.0, 0.75, 1.0});
    REQUIRE(field_preview);
    REQUIRE(field_preview.value().samples.at(1U).normalized_value == 0.75);

    const auto encoded = model.serialize();
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().validate());
    REQUIRE(restored.value().serialize() == encoded);
    REQUIRE(restored.value().developmental_program()->instructions().size() == 2U);
    REQUIRE(restored.value().developmental_trace()->entries().size() == 2U);
    REQUIRE(restored.value().developmental_trace()->program_digest == program_digest.value());
    REQUIRE(restored.value().developmental_trace()->genome_digest == genome_digest.value());
    REQUIRE(restored.value().developmental_trace()->entries().at(TraceId{1U}).seed ==
            std::uint64_t{101U});
    REQUIRE(restored.value().developmental_trace()->entries().at(TraceId{1U}).result_digest ==
            result_digest_a.value());
    REQUIRE(restored.value().phenotype_capabilities()->capabilities().size() == 1U);
    REQUIRE(restored.value().gene_regulatory_network()->expressions().size() == 1U);
    REQUIRE(restored.value().field_visualizations()->visualizations().size() == 1U);

    auto stale_program_evidence = model;
    auto stale_program_trace = *stale_program_evidence.developmental_trace();
    stale_program_trace.program_digest = result_digest_a.value();
    REQUIRE(stale_program_evidence.set_developmental_trace(std::move(stale_program_trace)));
    REQUIRE(!stale_program_evidence.validate());

    auto stale_genome_evidence = model;
    auto stale_genome_trace = *stale_genome_evidence.developmental_trace();
    stale_genome_trace.genome_digest = result_digest_b.value();
    REQUIRE(stale_genome_evidence.set_developmental_trace(std::move(stale_genome_trace)));
    REQUIRE(!stale_genome_evidence.validate());

    auto zero_result_trace = *model.developmental_trace();
    auto zero_result_entry = zero_result_trace.entries().at(TraceId{1U});
    zero_result_entry.id = TraceId{3U};
    zero_result_entry.result_digest = carto::assets::Sha256Digest{};
    REQUIRE(!zero_result_trace.insert_entry(std::move(zero_result_entry)));
}

void morphology_cross_domain_archetypes_round_trip() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();

    const auto make_archetype = [&](std::string identity,
                                    std::string body_plan,
                                    std::vector<std::string> axes,
                                    std::string symmetry,
                                    std::string root_class,
                                    std::string child_class,
                                    std::string relationship,
                                    std::string geometry_kind) {
        MorphologyModel morphology;
        morphology.organism_identity = std::move(identity);
        morphology.body_plan = std::move(body_plan);
        morphology.axes = std::move(axes);
        morphology.symmetry = std::move(symmetry);
        REQUIRE(morphology.insert_structure(MorphologyStructure{
            StructureId{1U}, std::move(root_class), std::nullopt, "midline", "stage:0",
            GeometryBinding{geometry_kind, "source:root"}, "organ-local", {}, "authored",
            provenance}));
        REQUIRE(morphology.insert_structure(MorphologyStructure{
            StructureId{2U}, std::move(child_class), StructureId{1U}, "right", "stage:1",
            GeometryBinding{geometry_kind, "source:child"}, "organ-local", {}, "authored",
            provenance}));
        REQUIRE(morphology.insert_attachment(MorphologyAttachment{
            AttachmentId{1U}, StructureId{1U}, StructureId{2U}, std::move(relationship),
            provenance}));
        REQUIRE(morphology.insert_growth_domain(MorphologyGrowthDomain{
            GrowthDomainId{1U}, StructureId{2U}, std::nullopt, {0.0, 1.0, 0.0}, 0.25,
            {1.0, 0.75, 0.5}, 4.0, "stage:1", "stage:2", provenance}));
        return morphology;
    };

    const std::vector<MorphologyModel> archetypes = {
        make_archetype("human-organ", "organ-hierarchy",
                       {"proximal-distal", "medial-lateral", "superior-inferior"},
                       "bilateral", "organ", "tissue", "embedded", "mesh"),
        make_archetype("plant-root", "root-branching", {"root-tip", "radial"},
                       "radial", "primary-root", "lateral-root", "branched", "curve"),
        make_archetype("insect-limb", "segmented-limb", {"proximal-distal", "dorsal-ventral"},
                       "bilateral", "limb", "segment", "articulated", "surface"),
        make_archetype("vascular-tree", "branching-network", {"flow", "radial"},
                       "branching", "vessel", "branch", "vascular", "implicit"),
    };

    for (const auto& expected : archetypes) {
        REQUIRE(expected.validate());
        const auto encoded = expected.serialize();
        REQUIRE(encoded == expected.serialize());
        const auto restored = MorphologyModel::deserialize(encoded);
        REQUIRE(restored);
        REQUIRE(restored.value().organism_identity == expected.organism_identity);
        REQUIRE(restored.value().body_plan == expected.body_plan);
        REQUIRE(restored.value().structures().size() == 2U);
        REQUIRE(restored.value().attachments().size() == 1U);
        REQUIRE(restored.value().growth_domains().size() == 1U);
        REQUIRE(restored.value().serialize() == encoded);
    }

    MorphologyModel cyclic;
    cyclic.organism_identity = "cycle-boundary";
    cyclic.body_plan = "generic";
    REQUIRE(cyclic.insert_structure(MorphologyStructure{
        StructureId{1U}, "root", StructureId{2U}, "", "stage:0", {}, "local", {},
        "authored", provenance}));
    REQUIRE(cyclic.insert_structure(MorphologyStructure{
        StructureId{2U}, "child", StructureId{1U}, "", "stage:0", {}, "local", {},
        "authored", provenance}));
    REQUIRE(!cyclic.validate());
}

void evolutionary_morphospace_preview_is_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();

    MorphometricModel morphometrics;
    const std::vector<std::array<double, 3>> signature_components{
        {0.0, 0.0, 0.0},
        {1.0, 2.0, 1.0},
        {2.0, 4.0, 2.0},
        {3.0, 6.0, 3.0},
    };
    for (std::size_t index = 0U; index < signature_components.size(); ++index) {
        REQUIRE(morphometrics.insert_signature(MorphologySignature{
            SignatureId{static_cast<std::uint64_t>(index + 1U)}, StructureId{2U},
            "shape-components", "organ-local", "authored",
            {signature_components[index][0U], signature_components[index][1U],
             signature_components[index][2U]},
            {"length", "width", "depth"}, provenance}));
    }
    REQUIRE(model.set_morphometrics(std::move(morphometrics)));

    LineageGraph lineage;
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, "founder-a"}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{2U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, "founder-b"}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{3U}, LineageId{1U}, std::nullopt, 1U, std::nullopt, std::nullopt,
        provenance, "child-a"}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{4U}, LineageId{2U}, LineageId{3U}, 2U, std::nullopt, std::nullopt,
        provenance, "recombined"}));
    REQUIRE(model.set_lineage_graph(std::move(lineage)));
    REQUIRE(model.validate());

    const std::vector<EvolutionaryMorphospaceBinding> bindings{
        {LineageId{4U}, SignatureId{4U}},
        {LineageId{2U}, SignatureId{2U}},
        {LineageId{1U}, SignatureId{1U}},
        {LineageId{3U}, SignatureId{3U}},
    };
    const auto preview = model.preview_evolutionary_morphospace(bindings, 2U, "unit-box");
    REQUIRE(preview);
    REQUIRE(preview.value().projection == "leading-components");
    REQUIRE(preview.value().normalization == "unit-box");
    REQUIRE(preview.value().dimensions == 2U);
    const std::vector<std::string> expected_labels{"length", "width"};
    REQUIRE(preview.value().component_labels == expected_labels);
    REQUIRE(preview.value().points.size() == 4U);
    REQUIRE(preview.value().points.at(0U).lineage == LineageId{1U});
    REQUIRE(preview.value().points.at(1U).position.x == 1.0 / 3.0);
    REQUIRE(preview.value().points.at(2U).position.y == 2.0 / 3.0);
    REQUIRE(preview.value().points.at(3U).position.x == 1.0);
    REQUIRE(preview.value().points.at(3U).position.z == 0.0);
    REQUIRE(preview.value().trajectories.size() == 3U);
    REQUIRE(preview.value().trajectories.at(0U).parent == LineageId{1U});
    REQUIRE(preview.value().trajectories.at(0U).child == LineageId{3U});
    REQUIRE(preview.value().trajectories.at(1U).parent == LineageId{2U});
    REQUIRE(preview.value().trajectories.at(1U).child == LineageId{4U});
    REQUIRE(preview.value().trajectories.at(2U).parent == LineageId{3U});
    REQUIRE(preview.value().trajectories.at(2U).child == LineageId{4U});
    REQUIRE(preview.value().validate());

    const auto repeated = model.preview_evolutionary_morphospace(bindings, 2U, "unit-box");
    REQUIRE(repeated);
    REQUIRE(repeated.value().points.size() == preview.value().points.size());
    REQUIRE(repeated.value().trajectories.size() == preview.value().trajectories.size());
    for (std::size_t index = 0U; index < preview.value().points.size(); ++index) {
        REQUIRE(repeated.value().points.at(index).lineage == preview.value().points.at(index).lineage);
        REQUIRE(repeated.value().points.at(index).position.x == preview.value().points.at(index).position.x);
        REQUIRE(repeated.value().points.at(index).position.y == preview.value().points.at(index).position.y);
        REQUIRE(repeated.value().points.at(index).position.z == preview.value().points.at(index).position.z);
    }

    const auto encoded = model.serialize();
    const auto reopened = ScientificModel::deserialize(encoded);
    REQUIRE(reopened);
    const auto reopened_preview = reopened.value().preview_evolutionary_morphospace(
        bindings, 2U, "unit-box");
    REQUIRE(reopened_preview);
    REQUIRE(reopened_preview.value().points.at(3U).position.x == preview.value().points.at(3U).position.x);
    REQUIRE(reopened_preview.value().points.at(3U).position.y == preview.value().points.at(3U).position.y);
    REQUIRE(reopened_preview.value().points.at(3U).position.z == preview.value().points.at(3U).position.z);
    REQUIRE(reopened_preview.value().trajectories.size() == preview.value().trajectories.size());

    auto forged = preview.value();
    forged.trajectories.at(0U).distance += 1.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.points.at(0U).position.z = 0.25;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.component_labels = {"length"};
    REQUIRE(!forged.validate());

    const std::vector<EvolutionaryMorphospaceBinding> unknown_lineage{
        {LineageId{999U}, SignatureId{1U}}};
    REQUIRE(!model.preview_evolutionary_morphospace(unknown_lineage));
    const std::vector<EvolutionaryMorphospaceBinding> duplicate_lineage{
        {LineageId{1U}, SignatureId{1U}}, {LineageId{1U}, SignatureId{2U}}};
    REQUIRE(!model.preview_evolutionary_morphospace(duplicate_lineage));
    REQUIRE(!model.preview_evolutionary_morphospace(bindings, 4U, "unit-box"));
    REQUIRE(!model.preview_evolutionary_morphospace(bindings, 2U, "z-score"));

    auto mismatch = *model.morphometrics();
    REQUIRE(mismatch.insert_signature(MorphologySignature{
        SignatureId{5U}, StructureId{2U}, "shape-components", "organ-local", "different-normalization",
        {4.0, 8.0, 4.0}, {"length", "width", "depth"}, provenance}));
    REQUIRE(model.set_morphometrics(std::move(mismatch)));
    const std::vector<EvolutionaryMorphospaceBinding> mismatched_bindings{
        {LineageId{1U}, SignatureId{1U}},
        {LineageId{2U}, SignatureId{2U}},
        {LineageId{3U}, SignatureId{3U}},
        {LineageId{4U}, SignatureId{5U}},
    };
    REQUIRE(!model.preview_evolutionary_morphospace(mismatched_bindings));
}

void parameter_sweep_preview_is_deterministic_bounded_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    ScientificModel model;
    REQUIRE(model.insert_parameter(Parameter{
        ParameterId{1U}, "growth-rate", "1/s", 0.25, 0.0, 1.0, provenance}));
    REQUIRE(model.insert_parameter(Parameter{
        ParameterId{2U}, "expression-strength", "ratio", 0.5, 0.0, 1.0, provenance}));
    REQUIRE(model.insert_parameter(Parameter{
        ParameterId{3U}, "temperature", "K", 300.0, 0.0, 1000.0, provenance}));
    REQUIRE(model.insert_study(Study{
        StudyId{7U}, carto::core::Revision{11U}, {}, {}, {}, {}, {},
        {ParameterId{3U}, ParameterId{1U}, ParameterId{2U}},
        "deterministic-reference", "reference-provider", {}, "acceptance=parameter-sweep",
        std::nullopt, provenance}));
    REQUIRE(model.validate());

    const auto preview = model.preview_parameter_sweep(
        StudyId{7U},
        {ParameterSweepAxis{ParameterId{2U}, {0.75, 0.25}},
         ParameterSweepAxis{ParameterId{1U}, {1.0, 0.5, 0.0}}});
    REQUIRE(preview);
    REQUIRE(preview.value().validate());
    REQUIRE(preview.value().study == StudyId{7U});
    REQUIRE(preview.value().source_model_revision == carto::core::Revision{11U});
    REQUIRE(preview.value().strategy == "cartesian");
    REQUIRE(preview.value().base_assignments.size() == 3U);
    REQUIRE(preview.value().base_assignments.at(0U).parameter == ParameterId{1U});
    REQUIRE(preview.value().base_assignments.at(1U).parameter == ParameterId{2U});
    REQUIRE(preview.value().base_assignments.at(2U).parameter == ParameterId{3U});
    REQUIRE(preview.value().axes.at(0U).parameter == ParameterId{1U});
    REQUIRE((preview.value().axes.at(0U).values == std::vector<double>{0.0, 0.5, 1.0}));
    REQUIRE(preview.value().axes.at(1U).parameter == ParameterId{2U});
    REQUIRE((preview.value().axes.at(1U).values == std::vector<double>{0.25, 0.75}));
    REQUIRE(preview.value().cases.size() == 6U);
    REQUIRE(preview.value().cases.at(0U).at(0U).value == 0.0);
    REQUIRE(preview.value().cases.at(0U).at(1U).value == 0.25);
    REQUIRE(preview.value().cases.at(0U).at(2U).value == 300.0);
    REQUIRE(preview.value().cases.at(5U).at(0U).value == 1.0);
    REQUIRE(preview.value().cases.at(5U).at(1U).value == 0.75);
    REQUIRE(preview.value().cases.at(5U).at(2U).value == 300.0);

    const auto repeat = model.preview_parameter_sweep(
        StudyId{7U},
        {ParameterSweepAxis{ParameterId{1U}, {0.0, 0.5, 1.0}},
         ParameterSweepAxis{ParameterId{2U}, {0.25, 0.75}}});
    REQUIRE(repeat);
    REQUIRE(repeat.value().cases == preview.value().cases);

    auto forged = preview.value();
    forged.cases.at(0U).at(0U).value = 0.1;
    REQUIRE(!forged.validate());
    REQUIRE(!model.preview_parameter_sweep(
        StudyId{7U}, {ParameterSweepAxis{ParameterId{1U}, {-0.1, 0.5}}}));
    REQUIRE(!model.preview_parameter_sweep(
        StudyId{7U}, {ParameterSweepAxis{ParameterId{99U}, {0.0, 1.0}}}));
    REQUIRE(!model.preview_parameter_sweep(StudyId{7U}, {}));
    REQUIRE(!model.preview_parameter_sweep(
        StudyId{7U}, {ParameterSweepAxis{ParameterId{1U},
                                          {std::numeric_limits<double>::quiet_NaN()}}}));
    REQUIRE(!model.preview_parameter_sweep(
        StudyId{7U}, {ParameterSweepAxis{ParameterId{1U}, {0.0, 1.0}},
                      ParameterSweepAxis{ParameterId{1U}, {0.25, 0.75}}}));

    std::vector<double> dense_values;
    dense_values.reserve(128U);
    for (std::size_t index = 0U; index < 128U; ++index) {
        dense_values.push_back(static_cast<double>(index) / 127.0);
    }
    REQUIRE(!model.preview_parameter_sweep(
        StudyId{7U}, {ParameterSweepAxis{ParameterId{1U}, dense_values},
                      ParameterSweepAxis{ParameterId{2U}, dense_values}}));
}

void functional_genome_provenance_codec_preserves_v1_and_rejects_future() {
    using namespace carto::scientific;

    FunctionalGenome genome;
    genome.schema = "cartographer.functional-genome.v1";
    genome.developmental_program_reference = "program://fixture";
    genome.lineage_reference = "lineage://fixture/child";
    const auto digest = carto::assets::Sha256Digest::from_hex(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    REQUIRE(digest);
    genome.source_digest = digest.value();
    REQUIRE(genome.validate());

    const auto encoded = genome.serialize();
    REQUIRE(encoded.find("CARTOGRAPHER_FUNCTIONAL_GENOME 2") != std::string::npos);
    const auto restored = FunctionalGenome::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().lineage_reference == "lineage://fixture/child");
    REQUIRE(restored.value().source_digest.has_value());
    REQUIRE(restored.value().source_digest->hex() ==
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    const auto legacy_v1 = FunctionalGenome::deserialize(
        "CARTOGRAPHER_FUNCTIONAL_GENOME 1\n"
        "\"cartographer.functional-genome.v1\" \"program://fixture\"\n"
        "GENES 0\n"
        "END\n");
    REQUIRE(legacy_v1);
    REQUIRE(legacy_v1.value().lineage_reference.empty());
    REQUIRE(!legacy_v1.value().source_digest.has_value());

    std::string future = encoded;
    const auto version_start = future.find("CARTOGRAPHER_FUNCTIONAL_GENOME 2");
    REQUIRE(version_start != std::string::npos);
    future.replace(version_start, std::string("CARTOGRAPHER_FUNCTIONAL_GENOME 2").size(),
                   "CARTOGRAPHER_FUNCTIONAL_GENOME 3");
    REQUIRE(!FunctionalGenome::deserialize(future));

    FunctionalGenome hostile = genome;
    hostile.lineage_reference = "lineage://fixture\nforged";
    REQUIRE(!hostile.validate());
}

void functional_genome_exchange_preserves_semantics_and_rejects_tampering() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const auto source_digest = carto::assets::Sha256Digest::from_hex(
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    REQUIRE(source_digest);

    const auto model = make_model();
    REQUIRE(model.functional_genome().has_value());
    const auto exchange = FunctionalGenomeExchange::from_import(
        *model.functional_genome(),
        ScientificImportReceipt{
            ImportId{7U}, "menagerie.functional-genome.v1", "menagerie://fixture/genome/7",
            "menagerie", source_digest.value(), 2U, 2U, {}, {}, provenance,
            {{"gene:growth-rate", "preserved", "functional-gene:1", "stable semantic value"},
             {"gene:sensor-count", "preserved", "functional-gene:2", "stable semantic value"}}});
    REQUIRE(exchange);
    REQUIRE(exchange.value().validate());
    REQUIRE(!exchange.value().semantic_digest.is_zero());

    const auto encoded = exchange.value().serialize();
    REQUIRE(encoded.find("CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE 1") != std::string::npos);
    const auto restored = FunctionalGenomeExchange::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().genome.serialize() == exchange.value().genome.serialize());
    REQUIRE(restored.value().import_receipt.provider == "menagerie");
    REQUIRE(restored.value().import_receipt.preservation_report.size() == 2U);
    REQUIRE(restored.value().semantic_digest == exchange.value().semantic_digest);
    REQUIRE(restored.value().serialize() == encoded);

    FunctionalGenome edited = exchange.value().genome;
    auto edited_gene = edited.genes().at(GeneId{1U});
    edited_gene.value = 0.35;
    REQUIRE(edited.replace_gene(edited_gene));
    REQUIRE(edited.genes().at(GeneId{1U}).value == 0.35);
    REQUIRE(edited.remove_gene(GeneId{2U}));
    REQUIRE(edited.genes().size() == 1U);
    REQUIRE(!edited.remove_gene(GeneId{999U}));
    auto invalid_edit = edited.genes().at(GeneId{1U});
    invalid_edit.value = 2.0;
    REQUIRE(!edited.replace_gene(invalid_edit));
    REQUIRE(edited.genes().at(GeneId{1U}).value == 0.35);
    auto edited_exchange = FunctionalGenomeExchange::from_import(
        std::move(edited), exchange.value().import_receipt);
    REQUIRE(edited_exchange);
    REQUIRE(edited_exchange.value().semantic_digest != exchange.value().semantic_digest);
    const auto edited_round_trip = FunctionalGenomeExchange::deserialize(
        edited_exchange.value().serialize());
    REQUIRE(edited_round_trip);
    REQUIRE(edited_round_trip.value().genome.genes().size() == 1U);
    REQUIRE(edited_round_trip.value().genome.genes().at(GeneId{1U}).value == 0.35);

    FunctionalGenomeExchange forged_digest = exchange.value();
    forged_digest.semantic_digest = source_digest.value();
    REQUIRE(!forged_digest.validate());
    REQUIRE(forged_digest.serialize().empty());

    std::string tampered = encoded;
    const auto value_offset = tampered.find("0.25");
    REQUIRE(value_offset != std::string::npos);
    tampered.replace(value_offset, 4U, "0.5");
    REQUIRE(!FunctionalGenomeExchange::deserialize(tampered));

    std::string sequence_payload = encoded;
    const auto functional_header = sequence_payload.find("CARTOGRAPHER_FUNCTIONAL_GENOME 2");
    REQUIRE(functional_header != std::string::npos);
    sequence_payload.replace(functional_header,
                             std::string("CARTOGRAPHER_FUNCTIONAL_GENOME 2").size(),
                             "CARTOGRAPHER_SEQUENCE_GENOME 1");
    REQUIRE(!FunctionalGenomeExchange::deserialize(sequence_payload));

    ScientificImportReceipt no_digest{
        ImportId{8U}, "menagerie.functional-genome.v1", "menagerie://fixture/genome/8",
        "menagerie", std::nullopt, 1U, 1U, {}, {}, provenance,
        {{"gene:growth-rate", "preserved", "functional-gene:1", "stable semantic value"}}};
    REQUIRE(!FunctionalGenomeExchange::from_import(
        *model.functional_genome(), std::move(no_digest)));

    std::string future = encoded;
    const auto exchange_header = future.find("CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE 1");
    REQUIRE(exchange_header != std::string::npos);
    future.replace(exchange_header,
                   std::string("CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE 1").size(),
                   "CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE 2");
    REQUIRE(!FunctionalGenomeExchange::deserialize(future));
}

void developmental_program_and_lineage_round_trip_with_boundaries() {
    using namespace carto::scientific;
    auto model = make_model();
    const auto provenance = fixture_provenance();

    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://fixture-development";
    program.description = "bounded authoring program";
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "specification", "always", provenance}));
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{2U}, 1U, "growth", "growth-rate > 0", provenance}));
    REQUIRE(program.insert_instruction(DevelopmentInstruction{
        InstructionId{1U}, StageId{1U}, "instantiate", "morphogenesis", "always",
        {ParameterId{1U}}, {StructureId{1U}}, {}, provenance}));
    REQUIRE(program.insert_instruction(DevelopmentInstruction{
        InstructionId{2U}, StageId{2U}, "expand", "morphogenesis", "growth-rate > 0",
        {ParameterId{1U}}, {}, {StructureId{2U}}, provenance}));
    REQUIRE(model.set_developmental_program(std::move(program)));

    AnatomyModel anatomy;
    anatomy.schema = "cartographer.anatomy.v1";
    REQUIRE(anatomy.insert_tissue_region(TissueRegion{
        TissueRegionId{1U}, "developmental-tissue", std::nullopt, RegionId{2U},
        "density=1", "program://fixture-development", "stage:1",
        GeometryBinding{"surface", "fixture:tissue"}, provenance}));
    REQUIRE(model.set_anatomy(std::move(anatomy)));

    DevelopmentalTrace trace;
    trace.program_reference = "program://fixture-development";
    trace.genome_reference = "program://fixture-development";
    REQUIRE(trace.insert_entry(DevelopmentTraceEntry{
        TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{1U}}, {},
        "receipt://development/1", provenance, TissueRegionId{1U}, {0.25},
        std::nullopt, std::nullopt}));
    REQUIRE(trace.insert_entry(DevelopmentTraceEntry{
        TraceId{2U}, StageId{2U}, InstructionId{2U}, {}, {StructureId{2U}},
        "receipt://development/2", provenance, std::nullopt, {0.75},
        std::nullopt, std::nullopt}));
    REQUIRE(model.set_developmental_trace(std::move(trace)));

    LineageGraph lineage;
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{2U}, LineageId{1U}, std::nullopt, 1U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{3U}, LineageId{1U}, LineageId{2U}, 2U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{4U}, LineageId{3U}, std::nullopt, 3U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(lineage.insert_mutation(MutationReceipt{
        MutationId{1U}, LineageId{1U}, LineageId{2U}, std::nullopt, std::nullopt, 42U,
        "point-value", {GeneId{1U}}, {InstructionId{2U}}, {"neural.bias=0.2"},
        "accepted-for-authoring-review", provenance}));
    REQUIRE(lineage.insert_mutation(MutationReceipt{
        MutationId{2U}, LineageId{3U}, LineageId{4U}, std::nullopt, std::nullopt, 43U,
        "post-recombination-point-value", {GeneId{2U}}, {InstructionId{1U}},
        {"neural.gain=1.1"}, "accepted-for-authoring-review", provenance}));
    REQUIRE(lineage.insert_recombination(RecombinationReceipt{
        RecombinationId{1U}, LineageId{1U}, LineageId{2U}, LineageId{3U}, std::nullopt,
        {"module:developmental", "module:sensory"}, MutationId{2U},
        "accepted-for-authoring-review", provenance}));
    REQUIRE(model.set_lineage_graph(std::move(lineage)));
    REQUIRE(model.validate());
    REQUIRE(model.developmental_trace()->entries().at(TraceId{1U}).tissue_region ==
            TissueRegionId{1U});
    REQUIRE(model.developmental_trace()->entries().at(TraceId{1U}).parameter_values ==
            std::vector<double>{0.25});
    const auto timeline = model.build_developmental_timeline();
    REQUIRE(timeline);
    REQUIRE(timeline.value().size() == 2U);
    REQUIRE(timeline.value().at(0U).stage == StageId{1U});
    REQUIRE(timeline.value().at(0U).instructions == std::vector<InstructionId>{InstructionId{1U}});
    REQUIRE(timeline.value().at(0U).trace_entries == std::vector<TraceId>{TraceId{1U}});
    REQUIRE(timeline.value().at(0U).planned_creates == std::vector<StructureId>{StructureId{1U}});
    REQUIRE(timeline.value().at(0U).observed_creates == std::vector<StructureId>{StructureId{1U}});
    REQUIRE(timeline.value().at(1U).stage == StageId{2U});
    REQUIRE(timeline.value().at(1U).planned_modifies == std::vector<StructureId>{StructureId{2U}});
    REQUIRE(timeline.value().at(1U).observed_modifies == std::vector<StructureId>{StructureId{2U}});
    const auto first_impact = model.build_developmental_instruction_impact(InstructionId{1U});
    REQUIRE(first_impact);
    REQUIRE(first_impact.value().stage == StageId{1U});
    REQUIRE(first_impact.value().planned_creates == std::vector<StructureId>{StructureId{1U}});
    REQUIRE(first_impact.value().planned_modifies.empty());
    REQUIRE(first_impact.value().trace_entries == std::vector<TraceId>{TraceId{1U}});
    REQUIRE(first_impact.value().observed_creates == std::vector<StructureId>{StructureId{1U}});
    const auto second_impact = model.build_developmental_instruction_impact(InstructionId{2U});
    REQUIRE(second_impact);
    REQUIRE(second_impact.value().planned_modifies == std::vector<StructureId>{StructureId{2U}});
    REQUIRE(second_impact.value().observed_modifies == std::vector<StructureId>{StructureId{2U}});
    REQUIRE(!model.build_developmental_instruction_impact(InstructionId{999U}));

    ScientificModel without_program;
    const auto empty_timeline = without_program.build_developmental_timeline();
    REQUIRE(empty_timeline);
    REQUIRE(empty_timeline.value().empty());
    REQUIRE(!without_program.build_developmental_instruction_impact(InstructionId{1U}));

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("DEVELOPMENTAL_PROGRAM 1") != std::string::npos);
    REQUIRE(encoded.find("DEVELOPMENTAL_TRACE 3") != std::string::npos);
    REQUIRE(encoded.find("LINEAGE_GRAPH 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().developmental_program().has_value());
    REQUIRE(restored.value().developmental_program()->instructions().size() == 2U);
    REQUIRE(restored.value().developmental_trace()->entries().size() == 2U);
    REQUIRE(restored.value().developmental_trace()->entries().at(TraceId{1U}).tissue_region ==
            TissueRegionId{1U});
    REQUIRE(restored.value().developmental_trace()->entries().at(TraceId{2U}).parameter_values ==
            std::vector<double>{0.75});
    REQUIRE(restored.value().lineage_graph()->nodes().size() == 4U);
    REQUIRE(restored.value().lineage_graph()->mutations().size() == 2U);
    REQUIRE(restored.value().lineage_graph()->recombinations().size() == 1U);
    REQUIRE(restored.value().lineage_graph()->recombinations().at(RecombinationId{1U}).post_mutation ==
            MutationId{2U});
    REQUIRE(restored.value().serialize() == encoded);

    DevelopmentalTrace legacy_trace;
    legacy_trace.program_reference = "program://fixture-development";
    legacy_trace.genome_reference = "program://fixture-development";
    REQUIRE(legacy_trace.insert_entry(DevelopmentTraceEntry{
        TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{1U}}, {},
        "receipt://development/legacy", provenance, std::nullopt, {},
        std::nullopt, std::nullopt}));
    std::string legacy_trace_text = legacy_trace.serialize();
    const auto legacy_trace_header = legacy_trace_text.find(
        "CARTOGRAPHER_DEVELOPMENTAL_TRACE 3");
    REQUIRE(legacy_trace_header != std::string::npos);
    legacy_trace_text.replace(
        legacy_trace_header, std::string("CARTOGRAPHER_DEVELOPMENTAL_TRACE 3").size(),
        "CARTOGRAPHER_DEVELOPMENTAL_TRACE 1");
    const auto legacy_trace_source_start = legacy_trace_text.find('\n') + 1U;
    const auto legacy_trace_source_end = legacy_trace_text.find('\n', legacy_trace_source_start);
    REQUIRE(legacy_trace_source_start != 0U && legacy_trace_source_end != std::string::npos);
    legacy_trace_text.replace(
        legacy_trace_source_start, legacy_trace_source_end - legacy_trace_source_start,
        "\"program://fixture-development\" \"program://fixture-development\"");
    const auto legacy_trace_metadata = legacy_trace_text.rfind(" 0 0 0 0\n");
    REQUIRE(legacy_trace_metadata != std::string::npos);
    legacy_trace_text.erase(legacy_trace_metadata, std::string(" 0 0 0 0").size());
    const auto legacy_trace_restored = DevelopmentalTrace::deserialize(legacy_trace_text);
    REQUIRE(legacy_trace_restored);
    REQUIRE(legacy_trace_restored.value().entries().at(TraceId{1U}).parameter_values.empty());

    auto invalid_parameter_trace_model = model;
    DevelopmentalTrace invalid_parameter_trace;
    invalid_parameter_trace.program_reference = "program://fixture-development";
    invalid_parameter_trace.genome_reference = "program://fixture-development";
    REQUIRE(invalid_parameter_trace.insert_entry(DevelopmentTraceEntry{
        TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{1U}}, {},
        "receipt://development/invalid-parameter-count", provenance, TissueRegionId{1U},
        {0.25, 0.5}, std::nullopt, std::nullopt}));
    REQUIRE(invalid_parameter_trace_model.set_developmental_trace(
        std::move(invalid_parameter_trace)));
    REQUIRE(!invalid_parameter_trace_model.validate());

    auto invalid_tissue_trace_model = model;
    DevelopmentalTrace invalid_tissue_trace;
    invalid_tissue_trace.program_reference = "program://fixture-development";
    invalid_tissue_trace.genome_reference = "program://fixture-development";
    REQUIRE(invalid_tissue_trace.insert_entry(DevelopmentTraceEntry{
        TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{1U}}, {},
        "receipt://development/invalid-tissue", provenance, TissueRegionId{999U}, {0.25},
        std::nullopt, std::nullopt}));
    REQUIRE(invalid_tissue_trace_model.set_developmental_trace(
        std::move(invalid_tissue_trace)));
    REQUIRE(!invalid_tissue_trace_model.validate());

    auto invalid_trace = *restored.value().developmental_trace();
    auto invalid_entry = invalid_trace.entries().at(TraceId{1U});
    invalid_entry.stage = StageId{2U};
    DevelopmentalTrace mismatch;
    mismatch.program_reference = "program://fixture-development";
    mismatch.genome_reference = "program://fixture-development";
    REQUIRE(mismatch.insert_entry(invalid_entry));
    auto mismatch_model = make_model();
    DevelopmentalProgram mismatch_program;
    mismatch_program.schema = "cartographer.developmental-program.v1";
    mismatch_program.reference = "program://fixture-development";
    REQUIRE(mismatch_program.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "specification", "always", provenance}));
    REQUIRE(mismatch_program.insert_stage(DevelopmentStage{
        StageId{2U}, 1U, "growth", "always", provenance}));
    REQUIRE(mismatch_program.insert_instruction(DevelopmentInstruction{
        InstructionId{1U}, StageId{1U}, "instantiate", "morphogenesis", "always",
        {ParameterId{1U}}, {StructureId{1U}}, {}, provenance}));
    REQUIRE(mismatch_model.set_developmental_program(std::move(mismatch_program)));
    REQUIRE(mismatch_model.set_developmental_trace(std::move(mismatch)));
    REQUIRE(!mismatch_model.validate());

    LineageGraph invalid_lineage;
    REQUIRE(invalid_lineage.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(!invalid_lineage.insert_node(LineageNode{
        LineageId{2U}, std::nullopt, std::nullopt, 1U, std::nullopt, std::nullopt,
        provenance, {}}));
}

void developmental_program_diff_is_structural_and_deterministic() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();

    DevelopmentalProgram parent;
    parent.schema = "cartographer.developmental-program.v1";
    parent.reference = "program://diff-parent";
    REQUIRE(parent.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "specification", "always", provenance}));
    REQUIRE(parent.insert_stage(DevelopmentStage{
        StageId{2U}, 1U, "growth", "rate > 0", provenance}));
    REQUIRE(parent.insert_instruction(DevelopmentInstruction{
        InstructionId{1U}, StageId{1U}, "instantiate", "morphogenesis", "always",
        {ParameterId{1U}}, {StructureId{1U}}, {}, provenance}));
    REQUIRE(parent.insert_instruction(DevelopmentInstruction{
        InstructionId{2U}, StageId{2U}, "branch", "branching", "rate > 0",
        {ParameterId{2U}}, {}, {StructureId{1U}}, provenance}));
    REQUIRE(parent.validate());

    DevelopmentalProgram child;
    child.schema = parent.schema;
    child.reference = "program://diff-child";
    child.source_digest = carto::assets::Sha256Digest::from_hex(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb").value();
    REQUIRE(child.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "specification-updated", "always", provenance}));
    REQUIRE(child.insert_stage(DevelopmentStage{
        StageId{3U}, 2U, "remodel", "remodeling > 0", provenance}));
    REQUIRE(child.insert_instruction(DevelopmentInstruction{
        InstructionId{1U}, StageId{1U}, "instantiate-updated", "growth", "always",
        {ParameterId{1U}, ParameterId{3U}}, {StructureId{1U}}, {StructureId{2U}}, provenance}));
    REQUIRE(child.insert_instruction(DevelopmentInstruction{
        InstructionId{3U}, StageId{3U}, "differentiate", "sensory", "always",
        {ParameterId{3U}}, {}, {StructureId{1U}}, provenance}));
    REQUIRE(child.validate());

    const auto child_serialized = child.serialize();
    const auto diff = child.diff_against(parent);
    REQUIRE(diff.added_stages == std::vector<StageId>{StageId{3U}});
    REQUIRE(diff.removed_stages == std::vector<StageId>{StageId{2U}});
    REQUIRE(diff.changed_stages == std::vector<StageId>{StageId{1U}});
    REQUIRE(diff.added_instructions == std::vector<InstructionId>{InstructionId{3U}});
    REQUIRE(diff.removed_instructions == std::vector<InstructionId>{InstructionId{2U}});
    REQUIRE(diff.changed_instructions == std::vector<InstructionId>{InstructionId{1U}});
    const std::vector<std::string> added_modules{"growth", "sensory"};
    const std::vector<std::string> removed_modules{"branching", "morphogenesis"};
    const std::vector<std::string> changed_modules{"growth", "morphogenesis"};
    REQUIRE(diff.added_modules == added_modules);
    REQUIRE(diff.removed_modules == removed_modules);
    REQUIRE(diff.changed_modules == changed_modules);
    REQUIRE(diff.source_identity_changed);
    REQUIRE(child.diff_against(parent).changed_instructions == diff.changed_instructions);
    REQUIRE(child.serialize() == child_serialized);
}

void developmental_and_lineage_deserialization_rejects_hostile_records() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    DevelopmentalProgram duplicate_ordinals;
    duplicate_ordinals.schema = "cartographer.developmental-program.v1";
    duplicate_ordinals.reference = "program://duplicate";
    REQUIRE(duplicate_ordinals.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "a", "", provenance}));
    REQUIRE(duplicate_ordinals.insert_stage(DevelopmentStage{
        StageId{2U}, 0U, "b", "", provenance}));
    REQUIRE(!duplicate_ordinals.validate());

    LineageGraph invalid_parent;
    REQUIRE(invalid_parent.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(invalid_parent.insert_node(LineageNode{
        LineageId{2U}, LineageId{1U}, std::nullopt, 1U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(invalid_parent.insert_mutation(MutationReceipt{
        MutationId{1U}, LineageId{2U}, LineageId{1U}, std::nullopt, std::nullopt, 0U,
        "bad-parent", {}, {}, {}, "must-reject", provenance}));
    REQUIRE(!invalid_parent.validate());

    ScientificModel model = make_model();
    const auto encoded = model.serialize();
    REQUIRE(!ScientificModel::deserialize(encoded + " trailing"));
    REQUIRE(!DevelopmentalProgram::deserialize(
        "CARTOGRAPHER_DEVELOPMENTAL_PROGRAM 999\nEND\n"));
    REQUIRE(!LineageGraph::deserialize(
        "CARTOGRAPHER_LINEAGE_GRAPH 1\nNODES 1\nNODE 1 0 0 1 0 0 "
        "\"fixture://x\" \"1\" \"CC0\" \"provider\" \"2026-10-03T00:00:00Z\" 0\n"
        "MUTATIONS 0\nEND\n"));
}

void lineage_newick_exchange_preserves_identity_and_fails_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    LineageGraph lineage;
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, "root"}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{2U}, LineageId{1U}, std::nullopt, 2U, std::nullopt, std::nullopt,
        provenance, "branch-a"}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{3U}, LineageId{1U}, std::nullopt, 1U, std::nullopt, std::nullopt,
        provenance, "branch-b"}));
    REQUIRE(lineage.insert_node(LineageNode{
        LineageId{4U}, LineageId{2U}, std::nullopt, 3U, std::nullopt, std::nullopt,
        provenance, "leaf"}));
    REQUIRE(lineage.insert_mutation(MutationReceipt{
        MutationId{1U}, LineageId{1U}, LineageId{2U}, std::nullopt, std::nullopt, 9U,
        "newick-fixture", {}, {}, {}, "accepted-for-authoring-review", provenance}));
    REQUIRE(lineage.validate());

    const auto exported = lineage.export_newick();
    REQUIRE(exported);
    REQUIRE(exported.value().text.find("#cartographer.newick_version=1\n") == 0U);
    REQUIRE(exported.value().text.find(
                "#cartographer.tree_kind=rooted-single-parent\n") != std::string::npos);
    REQUIRE(exported.value().text.find("cartographer__1__g0__") != std::string::npos);
    REQUIRE(exported.value().text.find("cartographer__2__g2__6272616e63682d61") !=
            std::string::npos);
    REQUIRE(exported.value().text.ends_with(";\n"));
    REQUIRE(exported.value().feature_loss_notes.size() == 4U);

    const auto imported = LineageGraph::import_newick(exported.value().text, provenance);
    REQUIRE(imported);
    REQUIRE(imported.value().nodes().size() == 4U);
    REQUIRE(imported.value().nodes().at(LineageId{2U}).parent_a == LineageId{1U});
    REQUIRE(imported.value().nodes().at(LineageId{2U}).generation == 2U);
    REQUIRE(imported.value().nodes().at(LineageId{2U}).source_identifier == "branch-a");
    REQUIRE(imported.value().mutations().empty());
    REQUIRE(imported.value().export_newick().value().text == exported.value().text);

    auto model = make_model();
    REQUIRE(model.set_lineage_graph(lineage));
    const auto restored = ScientificModel::deserialize(model.serialize());
    REQUIRE(restored);
    REQUIRE(restored.value().lineage_graph()->nodes().at(LineageId{1U}).source_identifier ==
            "root");
    REQUIRE(restored.value().serialize() == model.serialize());

    const auto legacy_v2 = LineageGraph::deserialize(
        "CARTOGRAPHER_LINEAGE_GRAPH 2\nNODES 1\n"
        "NODE 1 0 0 0 0 0 \"fixture://scientific-model\" \"2026.10\" "
        "\"CC0-1.0\" \"cartographer-test-provider\" \"2026-10-03T00:00:00Z\" 0\n"
        "MUTATIONS 0\nRECOMBINATIONS 0\nEND\n");
    REQUIRE(legacy_v2);
    REQUIRE(legacy_v2.value().nodes().at(LineageId{1U}).source_identifier.empty());

    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };
    REQUIRE(!LineageGraph::import_newick(exported.value().text, Provenance{}));
    REQUIRE(!LineageGraph::import_newick(
        replace_once(exported.value().text,
                     "cartographer__4__g3__6c656166", "cartographer__4__g3__6c656166:0"),
        provenance));
    REQUIRE(!LineageGraph::import_newick(
        replace_once(exported.value().text,
                     "cartographer__4__g3__6c656166", "cartographer__4__g3__6c656166[comment]"),
        provenance));
    REQUIRE(!LineageGraph::import_newick(
        replace_once(exported.value().text, "#cartographer.tree_kind=rooted-single-parent",
                     "#cartographer.tree_kind=network"),
        provenance));
    REQUIRE(!LineageGraph::import_newick(
        replace_once(exported.value().text, "#cartographer.newick_version=1\n",
                     "#cartographer.newick_version=1\n#tree comment\n"),
        provenance));

    std::string generic = exported.value().text;
    const auto body_start = generic.find("\n(");
    REQUIRE(body_start != std::string::npos);
    generic.erase(body_start + 1U);
    generic += "(A,B)root;\n";
    const auto generic_import = LineageGraph::import_newick(generic, provenance);
    REQUIRE(generic_import);
    REQUIRE(generic_import.value().nodes().size() == 3U);
    REQUIRE(generic_import.value().nodes().at(
                generic_import.value().nodes().begin()->first).generation == 0U);
    REQUIRE(std::any_of(
        generic_import.value().nodes().begin(), generic_import.value().nodes().end(),
        [](const auto& entry) { return entry.second.source_identifier == "A"; }));

    REQUIRE(!LineageGraph::import_newick(
        replace_once(exported.value().text,
                     "cartographer__3__g1__6272616e63682d62",
                     "cartographer__1__g1__6272616e63682d62"),
        provenance));
    LineageGraph reticulated;
    REQUIRE(reticulated.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, "root"}));
    REQUIRE(reticulated.insert_node(LineageNode{
        LineageId{2U}, LineageId{1U}, LineageId{3U}, 1U, std::nullopt, std::nullopt,
        provenance, "reticulation"}));
    REQUIRE(reticulated.insert_node(LineageNode{
        LineageId{3U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, "other-root"}));
    REQUIRE(reticulated.validate());
    REQUIRE(!reticulated.export_newick());
}

void morphometrics_round_trip_and_reference_checks() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();
    MorphometricModel morphometrics;
    REQUIRE(morphometrics.insert_observation(MorphometricObservation{
        MorphometricId{1U}, StructureId{1U}, "volume", "m3", 2.5,
        "provider://measurement/volume", provenance}));
    REQUIRE(morphometrics.insert_signature(MorphologySignature{
        SignatureId{1U}, StructureId{1U}, "landmark-distance", "organ-local", "unit-norm",
        {0.1, 0.2}, {"length", "width"}, provenance}));
    REQUIRE(morphometrics.insert_signature(MorphologySignature{
        SignatureId{2U}, StructureId{2U}, "landmark-distance", "organ-local", "unit-norm",
        {0.3, 0.4}, {"length", "width"}, provenance}));
    REQUIRE(morphometrics.insert_comparison(MorphologyComparison{
        ComparisonId{1U}, StructureId{1U}, StructureId{2U}, "cosine", 0.3, 0.7,
        SignatureId{1U}, SignatureId{2U}, provenance}));
    const auto signature_preview = morphometrics.preview_signature_comparison(
        SignatureId{1U}, SignatureId{2U});
    REQUIRE(signature_preview);
    REQUIRE(signature_preview.value().metric == "euclidean");
    REQUIRE(signature_preview.value().reference_structure == StructureId{1U});
    REQUIRE(signature_preview.value().candidate_structure == StructureId{2U});
    REQUIRE(std::abs(signature_preview.value().component_deltas.at(0U) - 0.2) < 1e-12);
    REQUIRE(std::abs(signature_preview.value().component_deltas.at(1U) - 0.2) < 1e-12);
    REQUIRE(std::abs(signature_preview.value().distance - std::sqrt(0.08)) < 1e-12);
    REQUIRE(std::abs(signature_preview.value().similarity -
                     (1.0 / (1.0 + std::sqrt(0.08)))) < 1e-12);
    auto forged_signature_preview = signature_preview.value();
    forged_signature_preview.component_deltas.at(0U) = 0.9;
    REQUIRE(!forged_signature_preview.validate());
    forged_signature_preview = signature_preview.value();
    forged_signature_preview.similarity = 0.0;
    REQUIRE(!forged_signature_preview.validate());
    REQUIRE(!morphometrics.preview_signature_comparison(SignatureId{999U}, SignatureId{2U}));
    REQUIRE(!morphometrics.preview_signature_comparison(SignatureId{1U}, SignatureId{1U}));
    REQUIRE(model.set_morphometrics(std::move(morphometrics)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("MORPHOMETRICS 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().morphometrics().has_value());
    REQUIRE(restored.value().morphometrics()->observations().size() == 1U);
    REQUIRE(restored.value().morphometrics()->signatures().at(SignatureId{2U}).components[1] == 0.4);
    REQUIRE(restored.value().serialize() == encoded);

    MorphometricModel non_finite;
    REQUIRE(!non_finite.insert_observation(MorphometricObservation{
        MorphometricId{1U}, StructureId{1U}, "volume", "m3",
        std::numeric_limits<double>::infinity(), "invalid", provenance}));
    REQUIRE(!non_finite.insert_signature(MorphologySignature{
        SignatureId{1U}, StructureId{1U}, "bad", "organ-local", "unit-norm",
        {0.1}, {"wrong-count", "extra"}, provenance}));

    ScientificModel missing_morphology;
    MorphometricModel orphan;
    REQUIRE(orphan.insert_observation(MorphometricObservation{
        MorphometricId{1U}, StructureId{99U}, "volume", "m3", 1.0,
        "provider://measurement", provenance}));
    REQUIRE(missing_morphology.set_morphometrics(std::move(orphan)));
    REQUIRE(!missing_morphology.validate());

    MorphometricModel wrong_signature;
    REQUIRE(wrong_signature.insert_signature(MorphologySignature{
        SignatureId{1U}, StructureId{1U}, "distance", "organ-local", "unit-norm",
        {1.0}, {"value"}, provenance}));
    REQUIRE(wrong_signature.insert_signature(MorphologySignature{
        SignatureId{2U}, StructureId{1U}, "distance", "organ-local", "unit-norm",
        {2.0}, {"value"}, provenance}));
    REQUIRE(wrong_signature.insert_comparison(MorphologyComparison{
        ComparisonId{1U}, StructureId{1U}, StructureId{2U}, "euclidean", 1.0, 0.5,
        SignatureId{1U}, SignatureId{2U}, provenance}));
    auto wrong_model = make_model();
    REQUIRE(!wrong_model.set_morphometrics(std::move(wrong_signature)));
}

void bilateral_morphology_landmark_preview_is_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    MorphologyModel morphology;
    morphology.organism_identity = "paired-fixture";
    morphology.body_plan = "bilateral";
    morphology.symmetry = "bilateral";
    morphology.axes = {"anterior-posterior", "dorsal-ventral"};
    REQUIRE(morphology.insert_structure(MorphologyStructure{
        StructureId{1U}, "appendage", std::nullopt, "left", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{1U}, LandmarkId{2U}}, "measured", provenance}));
    REQUIRE(morphology.insert_structure(MorphologyStructure{
        StructureId{2U}, "appendage", std::nullopt, "right", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{3U}, LandmarkId{4U}}, "measured", provenance}));
    REQUIRE(morphology.insert_landmark(MorphologyLandmark{
        LandmarkId{1U}, "base", StructureId{1U}, {0.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(morphology.insert_landmark(MorphologyLandmark{
        LandmarkId{2U}, "tip", StructureId{1U}, {0.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(morphology.insert_landmark(MorphologyLandmark{
        LandmarkId{3U}, "base", StructureId{2U}, {1.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(morphology.insert_landmark(MorphologyLandmark{
        LandmarkId{4U}, "tip", StructureId{2U}, {1.0, 1.5, 0.0}, "paired-local", provenance}));
    REQUIRE(morphology.validate());

    const auto preview = morphology.preview_bilateral_landmarks(StructureId{1U}, StructureId{2U});
    REQUIRE(preview);
    REQUIRE(preview.value().alignment_mode == "landmark");
    REQUIRE(preview.value().landmark_deltas.size() == 2U);
    REQUIRE(preview.value().landmark_deltas.at(0U).reference_landmark == LandmarkId{1U});
    REQUIRE(preview.value().landmark_deltas.at(0U).candidate_landmark == LandmarkId{3U});
    REQUIRE(preview.value().landmark_deltas.at(0U).displacement.x == 1.0);
    REQUIRE(preview.value().landmark_deltas.at(1U).distance == std::sqrt(1.25));
    REQUIRE(preview.value().rms_distance == std::sqrt(1.125));
    REQUIRE(preview.value().validate());
    const auto repeat = morphology.preview_bilateral_landmarks(StructureId{1U}, StructureId{2U});
    REQUIRE(repeat);
    REQUIRE(repeat.value().landmark_deltas.at(1U).displacement.y ==
            preview.value().landmark_deltas.at(1U).displacement.y);
    REQUIRE(!morphology.preview_bilateral_landmarks(StructureId{1U}, StructureId{1U}));
    REQUIRE(!morphology.preview_bilateral_landmarks(
        StructureId{1U}, StructureId{2U}, "rigid"));
    REQUIRE(!morphology.preview_bilateral_landmarks(
        StructureId{1U}, StructureId{2U}, "principal-axis"));
    auto forged = preview.value();
    forged.landmark_deltas.at(0U).distance = 999.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.alignment_translation.x = 1.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.alignment_mode = "rigid";
    forged.alignment_rotation.w = 2.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.landmark_deltas.at(0U).displacement.x = 2.0;
    forged.landmark_deltas.at(0U).distance = 2.0;
    REQUIRE(!forged.validate());

    MorphologyModel aligned;
    aligned.organism_identity = "aligned-fixture";
    aligned.body_plan = "bilateral";
    aligned.axes = {"x", "y", "z"};
    aligned.symmetry = "bilateral";
    REQUIRE(aligned.insert_structure(MorphologyStructure{
        StructureId{10U}, "reference", std::nullopt, "left", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{101U}, LandmarkId{102U}, LandmarkId{103U}, LandmarkId{104U}, LandmarkId{105U}},
        "measured", provenance}));
    REQUIRE(aligned.insert_structure(MorphologyStructure{
        StructureId{20U}, "candidate", std::nullopt, "right", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{201U}, LandmarkId{202U}, LandmarkId{203U}, LandmarkId{204U}, LandmarkId{205U}},
        "measured", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{101U}, "a", StructureId{10U}, {0.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{102U}, "b", StructureId{10U}, {1.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{103U}, "c", StructureId{10U}, {0.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{104U}, "d", StructureId{10U}, {0.0, 2.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{105U}, "e", StructureId{10U}, {0.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{201U}, "a", StructureId{20U}, {3.0, 4.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{202U}, "b", StructureId{20U}, {3.0, 5.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{203U}, "c", StructureId{20U}, {2.0, 4.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{204U}, "d", StructureId{20U}, {1.0, 4.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{205U}, "e", StructureId{20U}, {2.0, 4.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.validate());
    const std::vector<MorphologySurfaceSample> surface_samples{
        {LandmarkId{101U}, LandmarkId{201U}, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
         1.0, 1.5, 2.0, 2.25},
        {LandmarkId{102U}, LandmarkId{202U}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
         2.0, 1.75, 1.0, 0.5}};
    const auto surface_delta = aligned.preview_surface_delta(
        StructureId{10U}, StructureId{20U}, surface_samples);
    REQUIRE(surface_delta);
    REQUIRE(surface_delta.value().coordinate_frame == "paired-local");
    REQUIRE(surface_delta.value().deltas.size() == 2U);
    REQUIRE(surface_delta.value().deltas.at(0U).distance == 5.0);
    REQUIRE(surface_delta.value().deltas.at(0U).normal_displacement == 4.0);
    REQUIRE(surface_delta.value().deltas.at(0U).curvature_delta == 0.5);
    REQUIRE(surface_delta.value().deltas.at(0U).thickness_delta == 0.25);
    REQUIRE(surface_delta.value().deltas.at(1U).normal_angle == std::numbers::pi / 2.0);
    REQUIRE(std::abs(surface_delta.value().rms_displacement - std::sqrt(27.0)) < 1.0e-12);
    REQUIRE(surface_delta.value().validate());
    const auto surface_delta_repeat = aligned.preview_surface_delta(
        StructureId{10U}, StructureId{20U}, surface_samples);
    REQUIRE(surface_delta_repeat);
    REQUIRE(surface_delta_repeat.value().deltas.at(1U).normal_angle ==
            surface_delta.value().deltas.at(1U).normal_angle);
    REQUIRE(surface_delta_repeat.value().rms_displacement ==
            surface_delta.value().rms_displacement);
    auto forged_surface_delta = surface_delta.value();
    forged_surface_delta.deltas.at(0U).normal_angle = std::numbers::pi + 1.0;
    REQUIRE(!forged_surface_delta.validate());
    forged_surface_delta = surface_delta.value();
    forged_surface_delta.deltas.at(0U).normal_displacement =
        std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!forged_surface_delta.validate());
    REQUIRE(!aligned.preview_surface_delta(
        StructureId{10U}, StructureId{20U},
        std::vector<MorphologySurfaceSample>{surface_samples.front(), surface_samples.front()}));
    auto invalid_surface_sample = surface_samples.front();
    invalid_surface_sample.reference_normal = {0.0, 0.0, 0.0};
    REQUIRE(!aligned.preview_surface_delta(
        StructureId{10U}, StructureId{20U},
        std::vector<MorphologySurfaceSample>{invalid_surface_sample}));
    invalid_surface_sample = surface_samples.front();
    invalid_surface_sample.candidate_thickness = -1.0;
    REQUIRE(!aligned.preview_surface_delta(
        StructureId{10U}, StructureId{20U},
        std::vector<MorphologySurfaceSample>{invalid_surface_sample}));
    const std::vector<MorphologyLandmarkCorrespondence> explicit_correspondences{
        {LandmarkId{103U}, LandmarkId{203U}},
        {LandmarkId{101U}, LandmarkId{201U}},
        {LandmarkId{105U}, LandmarkId{205U}},
        {LandmarkId{104U}, LandmarkId{204U}},
        {LandmarkId{102U}, LandmarkId{202U}}};
    const auto explicit_rigid = aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U}, explicit_correspondences, "rigid");
    REQUIRE(explicit_rigid);
    REQUIRE(explicit_rigid.value().alignment_mode == "rigid");
    REQUIRE(explicit_rigid.value().rms_distance < 1.0e-12);
    REQUIRE(explicit_rigid.value().landmark_deltas.front().reference_landmark == LandmarkId{103U});
    REQUIRE(explicit_rigid.value().landmark_deltas.front().candidate_landmark == LandmarkId{203U});
    REQUIRE(!aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U},
        std::vector<MorphologyLandmarkCorrespondence>{
            {LandmarkId{101U}, LandmarkId{201U}},
            {LandmarkId{101U}, LandmarkId{202U}}},
        "landmark"));
    REQUIRE(!aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U},
        std::vector<MorphologyLandmarkCorrespondence>{{LandmarkId{201U}, LandmarkId{202U}}},
        "landmark"));
    REQUIRE(!aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U},
        std::vector<MorphologyLandmarkCorrespondence>{
            {LandmarkId{101U}, LandmarkId{201U}}},
        "rigid"));
    const std::vector<LandmarkId> distance_landmarks{LandmarkId{101U}, LandmarkId{102U}};
    const auto distance = aligned.preview_measurement(
        StructureId{10U}, "distance", distance_landmarks);
    REQUIRE(distance);
    REQUIRE(distance.value().units == "model");
    REQUIRE(distance.value().values.size() == 1U);
    REQUIRE(distance.value().values.front() == 1.0);
    const std::vector<LandmarkId> arc_landmarks{
        LandmarkId{101U}, LandmarkId{102U}, LandmarkId{104U}};
    const auto arc = aligned.preview_measurement(
        StructureId{10U}, "arc-length", arc_landmarks);
    REQUIRE(arc);
    REQUIRE(arc.value().units == "model");
    REQUIRE(std::abs(arc.value().values.front() - (1.0 + std::sqrt(5.0))) < 1.0e-12);
    const std::vector<LandmarkId> angle_landmarks{
        LandmarkId{102U}, LandmarkId{101U}, LandmarkId{103U}};
    const auto angle = aligned.preview_measurement(
        StructureId{10U}, "angle", angle_landmarks);
    REQUIRE(angle);
    REQUIRE(angle.value().units == "radians");
    REQUIRE(std::abs(angle.value().values.front() - (std::numbers::pi / 2.0)) < 1.0e-12);
    const auto branch_angle = aligned.preview_measurement(
        StructureId{10U}, "branch-angle", angle_landmarks);
    REQUIRE(branch_angle);
    REQUIRE(branch_angle.value().units == "radians");
    REQUIRE(std::abs(branch_angle.value().values.front() - (std::numbers::pi / 2.0)) < 1.0e-12);
    const std::vector<LandmarkId> triangle_landmarks{
        LandmarkId{101U}, LandmarkId{102U}, LandmarkId{103U}};
    const auto centroid = aligned.preview_measurement(
        StructureId{10U}, "centroid", triangle_landmarks);
    REQUIRE(centroid);
    REQUIRE(std::abs(centroid.value().values.at(0U) - (1.0 / 3.0)) < 1.0e-12);
    REQUIRE(std::abs(centroid.value().values.at(1U) - (1.0 / 3.0)) < 1.0e-12);
    const auto area = aligned.preview_measurement(
        StructureId{10U}, "area", triangle_landmarks);
    REQUIRE(area);
    REQUIRE(area.value().units == "model^2");
    REQUIRE(std::abs(area.value().values.front() - 0.5) < 1.0e-12);
    const auto principal_axes = aligned.preview_principal_axes(
        StructureId{10U}, triangle_landmarks);
    REQUIRE(principal_axes);
    REQUIRE(principal_axes.value().coordinate_frame == "paired-local");
    REQUIRE(std::abs(principal_axes.value().centroid.x - (1.0 / 3.0)) < 1.0e-12);
    REQUIRE(std::abs(principal_axes.value().centroid.y - (1.0 / 3.0)) < 1.0e-12);
    REQUIRE(principal_axes.value().variances.at(0U) > principal_axes.value().variances.at(1U));
    REQUIRE(principal_axes.value().variances.at(1U) > principal_axes.value().variances.at(2U));
    REQUIRE(std::abs(principal_axes.value().axes.at(0U).length() - 1.0) < 1.0e-12);
    REQUIRE(std::abs(principal_axes.value().axes.at(1U).length() - 1.0) < 1.0e-12);
    REQUIRE(std::abs(principal_axes.value().axes.at(2U).length() - 1.0) < 1.0e-12);
    REQUIRE(std::abs(carto::core::dot(principal_axes.value().axes.at(0U),
                                      principal_axes.value().axes.at(1U))) < 1.0e-12);
    REQUIRE(carto::core::dot(carto::core::cross(principal_axes.value().axes.at(0U),
                                                principal_axes.value().axes.at(1U)),
                             principal_axes.value().axes.at(2U)) > 0.999999999);
    const auto principal_axes_repeat = aligned.preview_principal_axes(
        StructureId{10U}, triangle_landmarks);
    REQUIRE(principal_axes_repeat);
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        REQUIRE(principal_axes_repeat.value().axes.at(axis).x ==
                principal_axes.value().axes.at(axis).x);
        REQUIRE(principal_axes_repeat.value().axes.at(axis).y ==
                principal_axes.value().axes.at(axis).y);
        REQUIRE(principal_axes_repeat.value().axes.at(axis).z ==
                principal_axes.value().axes.at(axis).z);
    }
    auto forged_principal_axes = principal_axes.value();
    forged_principal_axes.axes.at(0U).x = 2.0;
    REQUIRE(!forged_principal_axes.validate());
    REQUIRE(!aligned.preview_principal_axes(
        StructureId{10U}, std::vector<LandmarkId>{
            LandmarkId{101U}, LandmarkId{103U}, LandmarkId{104U}}));
    const auto dimensions = aligned.preview_measurement(
        StructureId{10U}, "bounding-dimensions", triangle_landmarks);
    REQUIRE(dimensions);
    REQUIRE((dimensions.value().values == std::vector<double>{1.0, 1.0, 0.0}));
    const std::vector<LandmarkId> ratio_landmarks{
        LandmarkId{101U}, LandmarkId{102U}, LandmarkId{103U}, LandmarkId{104U}};
    const auto ratio = aligned.preview_measurement(
        StructureId{10U}, "ratio", ratio_landmarks);
    REQUIRE(ratio);
    REQUIRE(ratio.value().units == "ratio");
    REQUIRE(ratio.value().values.front() == 1.0);

    MorphologyModel tetrahedron;
    tetrahedron.organism_identity = "tetrahedron-fixture";
    tetrahedron.body_plan = "explicit-landmark-volume";
    REQUIRE(tetrahedron.insert_structure(MorphologyStructure{
        StructureId{30U}, "tetrahedron", std::nullopt, "none", "stage:0", GeometryBinding{},
        "tetra-local", {LandmarkId{301U}, LandmarkId{302U}, LandmarkId{303U}, LandmarkId{304U}},
        "measured", provenance}));
    REQUIRE(tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{301U}, "origin", StructureId{30U}, {0.0, 0.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{302U}, "x", StructureId{30U}, {1.0, 0.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{303U}, "y", StructureId{30U}, {0.0, 1.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{304U}, "z", StructureId{30U}, {0.0, 0.0, 1.0},
        "tetra-local", provenance}));
    REQUIRE(tetrahedron.validate());
    const auto volume_landmarks = std::vector<LandmarkId>{
        LandmarkId{301U}, LandmarkId{302U}, LandmarkId{303U}, LandmarkId{304U}};
    const auto volume = tetrahedron.preview_measurement(
        StructureId{30U}, "volume", volume_landmarks);
    REQUIRE(volume);
    REQUIRE(volume.value().units == "model^3");
    REQUIRE(std::abs(volume.value().values.front() - (1.0 / 6.0)) < 1.0e-12);
    REQUIRE(!tetrahedron.preview_measurement(
        StructureId{30U}, "volume", std::vector<LandmarkId>{
            LandmarkId{301U}, LandmarkId{302U}, LandmarkId{303U}}));

    MorphologyModel degenerate_tetrahedron;
    degenerate_tetrahedron.organism_identity = "degenerate-tetrahedron-fixture";
    degenerate_tetrahedron.body_plan = "explicit-landmark-volume";
    REQUIRE(degenerate_tetrahedron.insert_structure(MorphologyStructure{
        StructureId{31U}, "coplanar-tetrahedron", std::nullopt, "none", "stage:0",
        GeometryBinding{}, "tetra-local",
        {LandmarkId{311U}, LandmarkId{312U}, LandmarkId{313U}, LandmarkId{314U}},
        "measured", provenance}));
    REQUIRE(degenerate_tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{311U}, "origin", StructureId{31U}, {0.0, 0.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(degenerate_tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{312U}, "x", StructureId{31U}, {1.0, 0.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(degenerate_tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{313U}, "y", StructureId{31U}, {0.0, 1.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(degenerate_tetrahedron.insert_landmark(MorphologyLandmark{
        LandmarkId{314U}, "xy", StructureId{31U}, {1.0, 1.0, 0.0},
        "tetra-local", provenance}));
    REQUIRE(degenerate_tetrahedron.validate());
    REQUIRE(!degenerate_tetrahedron.preview_measurement(
        StructureId{31U}, "volume", std::vector<LandmarkId>{
            LandmarkId{311U}, LandmarkId{312U}, LandmarkId{313U}, LandmarkId{314U}}));

    MorphologyModel nonplanar;
    nonplanar.organism_identity = "nonplanar-fixture";
    nonplanar.body_plan = "explicit-landmark-area";
    REQUIRE(nonplanar.insert_structure(MorphologyStructure{
        StructureId{40U}, "polygon", std::nullopt, "none", "stage:0", GeometryBinding{},
        "polygon-local", {LandmarkId{401U}, LandmarkId{402U}, LandmarkId{403U}, LandmarkId{404U}},
        "measured", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{401U}, "a", StructureId{40U}, {0.0, 0.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{402U}, "b", StructureId{40U}, {1.0, 0.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{403U}, "c", StructureId{40U}, {1.0, 1.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{404U}, "d", StructureId{40U}, {0.0, 1.0, 1.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_structure(MorphologyStructure{
        StructureId{41U}, "bowtie", std::nullopt, "none", "stage:0", GeometryBinding{},
        "polygon-local", {LandmarkId{411U}, LandmarkId{412U}, LandmarkId{413U}, LandmarkId{414U}},
        "measured", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{411U}, "a", StructureId{41U}, {0.0, 0.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{412U}, "b", StructureId{41U}, {1.0, 1.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{413U}, "c", StructureId{41U}, {0.0, 1.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.insert_landmark(MorphologyLandmark{
        LandmarkId{414U}, "d", StructureId{41U}, {1.0, 0.0, 0.0},
        "polygon-local", provenance}));
    REQUIRE(nonplanar.validate());
    REQUIRE(!nonplanar.preview_measurement(
        StructureId{40U}, "area", std::vector<LandmarkId>{
            LandmarkId{401U}, LandmarkId{402U}, LandmarkId{403U}, LandmarkId{404U}}));
    REQUIRE(!nonplanar.preview_measurement(
        StructureId{41U}, "area", std::vector<LandmarkId>{
            LandmarkId{411U}, LandmarkId{412U}, LandmarkId{413U}, LandmarkId{414U}}));
    auto forged_measurement = centroid.value();
    forged_measurement.values.at(0U) = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!forged_measurement.validate());
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "distance", std::vector<LandmarkId>{LandmarkId{101U}}));
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "arc-length", std::vector<LandmarkId>{LandmarkId{101U}}));
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "area", std::vector<LandmarkId>{
            LandmarkId{101U}, LandmarkId{102U}}));
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "distance", std::vector<LandmarkId>{LandmarkId{101U}, LandmarkId{201U}}));
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "unsupported", distance_landmarks));
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "ratio", std::vector<LandmarkId>{
            LandmarkId{101U}, LandmarkId{102U}, LandmarkId{103U}}));
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "ratio", std::vector<LandmarkId>{
            LandmarkId{101U}, LandmarkId{102U}, LandmarkId{103U}, LandmarkId{105U}}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{106U}, "unlisted", StructureId{10U}, {2.0, 2.0, 0.0},
        "paired-local", provenance}));
    REQUIRE(aligned.validate());
    REQUIRE(!aligned.preview_measurement(
        StructureId{10U}, "distance", std::vector<LandmarkId>{
            LandmarkId{101U}, LandmarkId{106U}}));
    const auto rigid = aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U}, "rigid");
    REQUIRE(rigid);
    REQUIRE(rigid.value().alignment_mode == "rigid");
    REQUIRE(rigid.value().alignment_scale == 1.0);
    REQUIRE(rigid.value().rms_distance < 1.0e-12);
    REQUIRE(rigid.value().alignment_rotation.normalizable());
    REQUIRE(rigid.value().alignment_translation.finite());
    const auto principal = aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U}, "principal-axis");
    REQUIRE(principal);
    REQUIRE(principal.value().alignment_mode == "principal-axis");
    REQUIRE(principal.value().alignment_scale == 1.0);
    REQUIRE(principal.value().rms_distance < 1.0e-10);
    const auto principal_repeat = aligned.preview_bilateral_landmarks(
        StructureId{10U}, StructureId{20U}, "principal-axis");
    REQUIRE(principal_repeat);
    REQUIRE(principal_repeat.value().alignment_rotation.x ==
            principal.value().alignment_rotation.x);
    REQUIRE(principal_repeat.value().alignment_rotation.y ==
            principal.value().alignment_rotation.y);
    REQUIRE(principal_repeat.value().alignment_rotation.z ==
            principal.value().alignment_rotation.z);
    REQUIRE(principal_repeat.value().alignment_rotation.w ==
            principal.value().alignment_rotation.w);

    REQUIRE(aligned.insert_structure(MorphologyStructure{
        StructureId{30U}, "reference-scaled", std::nullopt, "left", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{301U}, LandmarkId{302U}, LandmarkId{303U}},
        "measured", provenance}));
    REQUIRE(aligned.insert_structure(MorphologyStructure{
        StructureId{40U}, "candidate-scaled", std::nullopt, "right", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{401U}, LandmarkId{402U}, LandmarkId{403U}},
        "measured", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{301U}, "a", StructureId{30U}, {0.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{302U}, "b", StructureId{30U}, {1.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{303U}, "c", StructureId{30U}, {0.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{401U}, "a", StructureId{40U}, {5.0, 6.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{402U}, "b", StructureId{40U}, {5.0, 8.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{403U}, "c", StructureId{40U}, {3.0, 6.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.validate());
    const auto scale_normalized = aligned.preview_bilateral_landmarks(
        StructureId{30U}, StructureId{40U}, "scale-normalized");
    REQUIRE(scale_normalized);
    REQUIRE(scale_normalized.value().alignment_mode == "scale-normalized");
    REQUIRE(std::abs(scale_normalized.value().alignment_scale - 0.5) < 1.0e-12);
    REQUIRE(scale_normalized.value().rms_distance < 1.0e-12);

    // Equal principal moments are intentionally rejected because their axes
    // are not identifiable without a provider-owned tie-break.
    REQUIRE(aligned.insert_structure(MorphologyStructure{
        StructureId{50U}, "reference-symmetric", std::nullopt, "left", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{501U}, LandmarkId{502U}, LandmarkId{503U}, LandmarkId{504U}},
        "measured", provenance}));
    REQUIRE(aligned.insert_structure(MorphologyStructure{
        StructureId{60U}, "candidate-symmetric", std::nullopt, "right", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{601U}, LandmarkId{602U}, LandmarkId{603U}, LandmarkId{604U}},
        "measured", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{501U}, "a", StructureId{50U}, {-1.0, -1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{502U}, "b", StructureId{50U}, {1.0, -1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{503U}, "c", StructureId{50U}, {1.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{504U}, "d", StructureId{50U}, {-1.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{601U}, "a", StructureId{60U}, {-1.0, -1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{602U}, "b", StructureId{60U}, {1.0, -1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{603U}, "c", StructureId{60U}, {1.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.insert_landmark(MorphologyLandmark{
        LandmarkId{604U}, "d", StructureId{60U}, {-1.0, 1.0, 0.0}, "paired-local", provenance}));
    REQUIRE(aligned.validate());
    REQUIRE(!aligned.preview_bilateral_landmarks(
        StructureId{50U}, StructureId{60U}, "principal-axis"));
    REQUIRE(!aligned.preview_principal_axes(
        StructureId{50U}, std::vector<LandmarkId>{
            LandmarkId{501U}, LandmarkId{502U}, LandmarkId{503U}, LandmarkId{504U}}));

    // Rebuild the small hostile fixture because the model exposes only
    // read-only collections after admission.
    MorphologyModel missing_ownership;
    missing_ownership.organism_identity = "paired-fixture";
    missing_ownership.body_plan = "bilateral";
    REQUIRE(missing_ownership.insert_structure(MorphologyStructure{
        StructureId{1U}, "appendage", std::nullopt, "left", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{1U}}, "measured", provenance}));
    REQUIRE(missing_ownership.insert_structure(MorphologyStructure{
        StructureId{2U}, "appendage", std::nullopt, "right", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{2U}}, "measured", provenance}));
    REQUIRE(missing_ownership.insert_landmark(MorphologyLandmark{
        LandmarkId{1U}, "base", std::nullopt, {0.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(missing_ownership.insert_landmark(MorphologyLandmark{
        LandmarkId{2U}, "base", StructureId{2U}, {1.0, 0.0, 0.0}, "paired-local", provenance}));
    REQUIRE(missing_ownership.validate());
    REQUIRE(!missing_ownership.preview_bilateral_landmarks(StructureId{1U}, StructureId{2U}));

    // A frame mismatch is a hard boundary: no implicit transform is applied.
    MorphologyModel frame_hostile;
    frame_hostile.organism_identity = "paired-fixture";
    frame_hostile.body_plan = "bilateral";
    REQUIRE(frame_hostile.insert_structure(MorphologyStructure{
        StructureId{1U}, "appendage", std::nullopt, "left", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{1U}}, "measured", provenance}));
    REQUIRE(frame_hostile.insert_structure(MorphologyStructure{
        StructureId{2U}, "appendage", std::nullopt, "right", "stage:0", GeometryBinding{},
        "paired-local", {LandmarkId{2U}}, "measured", provenance}));
    REQUIRE(frame_hostile.insert_landmark(MorphologyLandmark{
        LandmarkId{1U}, "base", StructureId{1U}, {0.0, 0.0, 0.0}, "left-frame", provenance}));
    REQUIRE(frame_hostile.insert_landmark(MorphologyLandmark{
        LandmarkId{2U}, "base", StructureId{2U}, {1.0, 0.0, 0.0}, "right-frame", provenance}));
    REQUIRE(frame_hostile.validate());
    REQUIRE(!frame_hostile.preview_bilateral_landmarks(StructureId{1U}, StructureId{2U}));
}

void morphology_landmark_exchange_is_reference_bound_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    MorphologyModel source;
    source.organism_identity = "landmark-fixture";
    source.body_plan = "bilateral";
    source.axes = {"anterior-posterior", "dorsal-ventral"};
    source.symmetry = "bilateral";
    REQUIRE(source.insert_structure(MorphologyStructure{
        StructureId{1U}, "appendage", std::nullopt, "left", "stage:0",
        GeometryBinding{"surface", "mesh://appendage-left"}, "organ-local", {},
        "measured", provenance}));
    REQUIRE(source.insert_structure(MorphologyStructure{
        StructureId{2U}, "body", std::nullopt, "midline", "stage:0",
        GeometryBinding{}, "organ-local", {}, "authored", provenance}));
    REQUIRE(source.insert_landmark(MorphologyLandmark{
        LandmarkId{10U}, "tip point", StructureId{1U}, {1.0, 2.0, 3.0},
        "organ-local", provenance}));
    REQUIRE(source.insert_landmark(MorphologyLandmark{
        LandmarkId{11U}, "free point", std::nullopt, {-1.0, 0.5, 0.0},
        "world", provenance}));
    REQUIRE(source.validate());

    MorphologyModel reference;
    reference.organism_identity = source.organism_identity;
    reference.body_plan = source.body_plan;
    reference.axes = source.axes;
    reference.symmetry = source.symmetry;
    REQUIRE(reference.insert_structure(MorphologyStructure{
        StructureId{1U}, "appendage", std::nullopt, "left", "stage:0",
        GeometryBinding{"surface", "mesh://appendage-left"}, "organ-local", {},
        "measured", provenance}));
    REQUIRE(reference.insert_structure(MorphologyStructure{
        StructureId{2U}, "body", std::nullopt, "midline", "stage:0",
        GeometryBinding{}, "organ-local", {}, "authored", provenance}));
    REQUIRE(reference.validate());

    const auto exported = source.export_landmark_set();
    REQUIRE(exported);
    REQUIRE(exported.value().text.find("##fileformat=cartographer.landmarks.v1\n") == 0U);
    REQUIRE(exported.value().text.find("##cartographer.coordinate_frame_hex=mixed\n") !=
            std::string::npos);
    REQUIRE(exported.value().text.find(
                "#LANDMARK_ID\tSTRUCTURE_ID\tNAME_HEX\tX\tY\tZ\tFRAME_HEX\n") !=
            std::string::npos);
    REQUIRE(exported.value().feature_loss_notes.size() == 2U);

    const auto imported = MorphologyModel::import_landmark_set(
        exported.value().text, reference, provenance);
    REQUIRE(imported);
    REQUIRE(imported.value().validate());
    REQUIRE(imported.value().landmarks().size() == 2U);
    REQUIRE(imported.value().landmarks().at(LandmarkId{10U}).name == "tip point");
    REQUIRE(imported.value().landmarks().at(LandmarkId{11U}).structure == std::nullopt);
    REQUIRE(imported.value().structures().at(StructureId{1U}).landmarks ==
            std::vector<LandmarkId>{LandmarkId{10U}});
    REQUIRE(imported.value().export_landmark_set().value().text == exported.value().text);

    REQUIRE(!MorphologyModel::import_landmark_set(
        exported.value().text, source, provenance));
    REQUIRE(!MorphologyModel::import_landmark_set(
        exported.value().text, reference, Provenance{}));
    MorphologyModel wrong_identity = reference;
    wrong_identity.organism_identity = "different-organism";
    REQUIRE(!MorphologyModel::import_landmark_set(
        exported.value().text, wrong_identity, provenance));

    const auto unsupported_metadata = [&]() {
        std::string modified = exported.value().text;
        const auto marker = std::string("##fileformat=cartographer.landmarks.v1\n");
        modified.insert(marker.size(), "##source=external-tool\n");
        return MorphologyModel::import_landmark_set(modified, reference, provenance);
    }();
    REQUIRE(!unsupported_metadata);
    const auto duplicate_landmark = [&]() {
        std::string modified = exported.value().text;
        modified += "10\t1\t74697020706f696e74\t1\t2\t3\t6f7267616e2d6c6f63616c\n";
        return MorphologyModel::import_landmark_set(modified, reference, provenance);
    }();
    REQUIRE(!duplicate_landmark);
    const auto stale_structure = [&]() {
        std::string modified = exported.value().text;
        const auto marker = std::string("10\t1\t");
        modified.replace(modified.find(marker), marker.size(), "10\t99\t");
        return MorphologyModel::import_landmark_set(modified, reference, provenance);
    }();
    REQUIRE(!stale_structure);
    const auto non_finite_position = [&]() {
        std::string modified = exported.value().text;
        const auto marker = std::string("\t1\t2\t3\t");
        modified.replace(modified.find(marker), marker.size(), "\tnan\t2\t3\t");
        return MorphologyModel::import_landmark_set(modified, reference, provenance);
    }();
    REQUIRE(!non_finite_position);
    const auto frame_mismatch = [&]() {
        std::string modified = exported.value().text;
        const auto marker = std::string("##cartographer.coordinate_frame_hex=mixed\n");
        const auto value_start = std::string("##cartographer.coordinate_frame_hex=").size();
        modified.replace(value_start, 5U, "6f7267616e2d6c6f63616c");
        return MorphologyModel::import_landmark_set(modified, reference, provenance);
    }();
    REQUIRE(!frame_mismatch);
}

void morphology_growth_domain_round_trip_and_rejection() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    MorphologyModel morphology;
    morphology.organism_identity = "growth-fixture";
    morphology.body_plan = "branching";
    morphology.symmetry = "none";
    REQUIRE(morphology.insert_structure(MorphologyStructure{
        StructureId{1U}, "branch", std::nullopt, "unspecified", "stage:0",
        GeometryBinding{}, "growth-local", {}, "authored", provenance}));
    REQUIRE(!morphology.insert_growth_domain(MorphologyGrowthDomain{
        GrowthDomainId{1U}, StructureId{1U}, std::nullopt, {0.0, 0.0, 0.0}, 1.0,
        {1.0, 1.0, 1.0}, std::nullopt, "stage:0", "stage:1", provenance}));
    REQUIRE(!morphology.insert_growth_domain(MorphologyGrowthDomain{
        GrowthDomainId{1U}, StructureId{1U}, std::nullopt, {1.0, 0.0, 0.0}, -1.0,
        {1.0, 1.0, 1.0}, std::nullopt, "stage:0", "stage:1", provenance}));
    REQUIRE(!morphology.insert_growth_domain(MorphologyGrowthDomain{
        GrowthDomainId{1U}, StructureId{1U}, std::nullopt, {1.0, 0.0, 0.0}, 1.0,
        {0.0, 0.0, 0.0}, std::nullopt, "stage:0", "stage:1", provenance}));
    REQUIRE(morphology.insert_growth_domain(MorphologyGrowthDomain{
        GrowthDomainId{1U}, StructureId{1U}, FieldId{1U}, {1.0, 0.0, 0.0}, 1.0,
        {1.0, 0.5, 0.25}, 4.0, "stage:0", "stage:1", provenance}));
    REQUIRE(morphology.validate());
    const auto encoded = morphology.serialize();
    REQUIRE(encoded.find("CARTOGRAPHER_MORPHOLOGY 2") != std::string::npos);
    const auto restored = MorphologyModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().growth_domains().at(GrowthDomainId{1U}).maximum_extent == 4.0);
    REQUIRE(restored.value().serialize() == encoded);

    std::string legacy = encoded;
    const auto header = legacy.find("CARTOGRAPHER_MORPHOLOGY 2");
    REQUIRE(header != std::string::npos);
    legacy.replace(header, std::string("CARTOGRAPHER_MORPHOLOGY 2").size(),
                   "CARTOGRAPHER_MORPHOLOGY 1");
    const auto growth_section = legacy.find("GROWTH_DOMAINS 1\n");
    REQUIRE(growth_section != std::string::npos);
    const auto end = legacy.find("END\n", growth_section);
    REQUIRE(end != std::string::npos);
    legacy.erase(growth_section, end - growth_section);
    const auto legacy_restored = MorphologyModel::deserialize(legacy);
    REQUIRE(legacy_restored);
    REQUIRE(legacy_restored.value().growth_domains().empty());

    auto orphan_model = make_model();
    MorphologyModel orphan_morphology;
    orphan_morphology.organism_identity = "orphan-growth";
    orphan_morphology.body_plan = "branching";
    orphan_morphology.symmetry = "none";
    REQUIRE(orphan_morphology.insert_structure(MorphologyStructure{
        StructureId{1U}, "branch", std::nullopt, "unspecified", "stage:0",
        GeometryBinding{}, "growth-local", {}, "authored", provenance}));
    REQUIRE(orphan_morphology.insert_growth_domain(MorphologyGrowthDomain{
        GrowthDomainId{1U}, StructureId{1U}, FieldId{999U}, {1.0, 0.0, 0.0}, 1.0,
        {1.0, 1.0, 1.0}, std::nullopt, "stage:0", "stage:1", provenance}));
    REQUIRE(orphan_model.set_morphology(std::move(orphan_morphology)));
    REQUIRE(!orphan_model.validate());
}

void sequence_genome_round_trip_and_reference_checks() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();
    SequenceGenome genome;
    genome.schema = "cartographer.sequence-genome.v1";
    genome.assembly = "fixture-assembly";
    genome.sample_reference = "sample://fixture";
    REQUIRE(genome.insert_contig(SequenceContig{
        ContigId{1U}, "chr-fixture", "AACGTNRY", std::nullopt, provenance}));
    REQUIRE(genome.insert_variant(SequenceVariant{
        VariantId{1U}, ContigId{1U}, 2U, "CG", "TT", "substitution", provenance, ""}));
    REQUIRE(model.set_sequence_genome(std::move(genome)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("SEQUENCE_GENOME 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().sequence_genome().has_value());
    REQUIRE(restored.value().sequence_genome()->contigs().at(ContigId{1U}).sequence == "AACGTNRY");
    REQUIRE(restored.value().sequence_genome()->variants().at(VariantId{1U}).position == 2U);
    REQUIRE(restored.value().serialize() == encoded);

    const auto fasta = restored.value().sequence_genome()->export_fasta(4U);
    REQUIRE(fasta);
    REQUIRE(fasta.value().text ==
            ">cartographer.sequence.v1|id=1|name_hex=6368722d66697874757265\n"
            "AACG\nTNRY\n");
    REQUIRE(fasta.value().feature_loss_notes.size() == 2U);
    REQUIRE(fasta.value().feature_loss_notes.at(1U).find("1 explicit sequence variants") !=
            std::string::npos);
    const auto imported_fasta = SequenceGenome::import_fasta(
        fasta.value().text, "fasta-assembly", "sample://fasta", provenance);
    REQUIRE(imported_fasta);
    REQUIRE(imported_fasta.value().contigs().at(ContigId{1U}).name == "chr-fixture");
    REQUIRE(imported_fasta.value().contigs().at(ContigId{1U}).sequence == "AACGTNRY");
    REQUIRE(imported_fasta.value().contigs().at(ContigId{1U}).content_digest.has_value());
    REQUIRE(imported_fasta.value().variants().empty());
    REQUIRE(imported_fasta.value().export_fasta(4U).value().text == fasta.value().text);

    const auto generic_fasta = SequenceGenome::import_fasta(
        ">external chr one description\nACGT\n>second\nNN\n",
        "generic-assembly", "sample://generic", provenance);
    REQUIRE(generic_fasta);
    REQUIRE(generic_fasta.value().contigs().at(ContigId{1U}).name ==
            "external chr one description");
    REQUIRE(generic_fasta.value().contigs().at(ContigId{2U}).sequence == "NN");
    REQUIRE(!SequenceGenome::import_fasta("ACGT\n", "assembly", "sample", provenance));
    REQUIRE(!SequenceGenome::import_fasta(
        ">empty\n>next\nACGT\n", "assembly", "sample", provenance));
    REQUIRE(!SequenceGenome::import_fasta(
        ">external\nacgt\n", "assembly", "sample", provenance));
    REQUIRE(!SequenceGenome::import_fasta(
        ">cartographer.sequence.v1|id=1|name_hex=zz\nACGT\n",
        "assembly", "sample", provenance));
    const auto large_stable_id = SequenceGenome::import_fasta(
        ">cartographer.sequence.v1|id=1844674407370955161|name_hex=6368722d6c61726765\nACGT\n",
        "assembly", "sample", provenance);
    REQUIRE(large_stable_id);
    REQUIRE(large_stable_id.value().contigs().contains(ContigId{1844674407370955161U}));
    REQUIRE(!restored.value().sequence_genome()->export_fasta(0U));

    const auto reference_digest = carto::assets::Sha256Digest::from_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    REQUIRE(reference_digest);
    SequenceGenome vcf_reference;
    vcf_reference.schema = "cartographer.sequence-genome.v1";
    vcf_reference.assembly = "fixture-assembly";
    vcf_reference.sample_reference = "sample://fixture";
    vcf_reference.source_digest = reference_digest.value();
    REQUIRE(vcf_reference.insert_contig(SequenceContig{
        ContigId{1U}, "chr-fixture", "AACGTNRY", std::nullopt, provenance}));
    auto vcf_genome = vcf_reference;
    REQUIRE(vcf_genome.insert_variant(SequenceVariant{
        VariantId{1U}, ContigId{1U}, 2U, "CG", "TT", "substitution", provenance,
        "rs-fixture"}));
    const auto vcf = vcf_genome.export_vcf();
    REQUIRE(vcf);
    REQUIRE(vcf.value().text.find("##fileformat=VCFv4.3\n") == 0U);
    REQUIRE(vcf.value().text.find(
                "##cartographer.reference_digest=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n") !=
            std::string::npos);
    REQUIRE(vcf.value().text.find(
                "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n") != std::string::npos);
    REQUIRE(vcf.value().text.find(
                "chr-fixture\t3\trs-fixture\tCG\tTT\t.\tPASS\tCARTOGRAPHER_KIND_HEX=737562737469747574696f6e\n") !=
            std::string::npos);
    REQUIRE(vcf.value().feature_loss_notes.size() == 3U);
    const auto imported_vcf = SequenceGenome::import_vcf(
        vcf.value().text, vcf_reference, provenance);
    REQUIRE(imported_vcf);
    REQUIRE(imported_vcf.value().variants().size() == 1U);
    const auto& imported_variant = imported_vcf.value().variants().begin()->second;
    REQUIRE(imported_variant.position == 2U);
    REQUIRE(imported_variant.source_identifier == "rs-fixture");
    REQUIRE(imported_variant.kind == "substitution");
    REQUIRE(imported_vcf.value().export_vcf().value().text == vcf.value().text);

    auto canonical_vcf_genome = vcf_reference;
    REQUIRE(canonical_vcf_genome.insert_variant(SequenceVariant{
        VariantId{7U}, ContigId{1U}, 2U, "CG", "TT", "substitution", provenance, ""}));
    const auto canonical_vcf = canonical_vcf_genome.export_vcf();
    REQUIRE(canonical_vcf);
    REQUIRE(canonical_vcf.value().text.find("\t3\tcartographer:7\tCG\tTT\t") !=
            std::string::npos);
    const auto canonical_import = SequenceGenome::import_vcf(
        canonical_vcf.value().text, vcf_reference, provenance);
    REQUIRE(canonical_import);
    REQUIRE(canonical_import.value().variants().contains(VariantId{7U}));
    REQUIRE(canonical_import.value().variants().at(VariantId{7U}).source_identifier.empty());
    auto reserved_identifier_genome = vcf_reference;
    REQUIRE(reserved_identifier_genome.insert_variant(SequenceVariant{
        VariantId{8U}, ContigId{1U}, 2U, "CG", "TT", "substitution", provenance,
        "cartographer:external"}));
    REQUIRE(!reserved_identifier_genome.export_vcf());

    const auto empty_provenance = Provenance{};
    REQUIRE(!SequenceGenome::import_vcf(vcf.value().text, vcf_reference, empty_provenance));
    auto wrong_reference = vcf_reference;
    wrong_reference.source_digest = carto::assets::Sha256Digest::from_hex(
        "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd").value();
    REQUIRE(!SequenceGenome::import_vcf(vcf.value().text, wrong_reference, provenance));
    REQUIRE(!SequenceGenome::import_vcf(
        vcf.value().text, vcf_genome, provenance));
    const auto missing_coordinate = [&]() {
        std::string modified = vcf.value().text;
        const auto marker = std::string("##cartographer.coordinate_system=vcf-1-based\n");
        modified.erase(modified.find(marker), marker.size());
        return SequenceGenome::import_vcf(modified, vcf_reference, provenance);
    }();
    REQUIRE(!missing_coordinate);
    const auto unsupported_info = [&]() {
        std::string modified = vcf.value().text;
        const auto marker = std::string("CARTOGRAPHER_KIND_HEX=737562737469747574696f6e");
        modified.replace(modified.find(marker), marker.size(), "DP=4");
        return SequenceGenome::import_vcf(modified, vcf_reference, provenance);
    }();
    REQUIRE(!unsupported_info);
    const auto unsupported_metadata = [&]() {
        std::string modified = vcf.value().text;
        const auto marker = std::string("##fileformat=VCFv4.3\n");
        modified.insert(marker.size(), "##source=external-tool\n");
        return SequenceGenome::import_vcf(modified, vcf_reference, provenance);
    }();
    REQUIRE(!unsupported_metadata);
    const auto extra_sample_column = [&]() {
        std::string modified = vcf.value().text;
        const auto header = std::string("#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n");
        modified.replace(modified.find(header), header.size(),
                         "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tSAMPLE\n");
        return SequenceGenome::import_vcf(modified, vcf_reference, provenance);
    }();
    REQUIRE(!extra_sample_column);
    const auto stale_reference_allele = [&]() {
        std::string modified = vcf.value().text;
        const auto marker = std::string("\t3\trs-fixture\tCG\tTT\t");
        modified.replace(modified.find(marker), marker.size(), "\t3\trs-fixture\tAA\tTT\t");
        return SequenceGenome::import_vcf(modified, vcf_reference, provenance);
    }();
    REQUIRE(!stale_reference_allele);
    const auto lowercase_allele = [&]() {
        std::string modified = vcf.value().text;
        const auto marker = std::string("\tCG\tTT\t");
        modified.replace(modified.find(marker), marker.size(), "\tcg\tTT\t");
        return SequenceGenome::import_vcf(modified, vcf_reference, provenance);
    }();
    REQUIRE(!lowercase_allele);

    std::string legacy_v2 = canonical_vcf_genome.serialize();
    const auto current_codec_header = std::string("CARTOGRAPHER_SEQUENCE_GENOME 3");
    const auto legacy_codec_header = std::string("CARTOGRAPHER_SEQUENCE_GENOME 2");
    REQUIRE(legacy_v2.find(current_codec_header) != std::string::npos);
    legacy_v2.replace(legacy_v2.find(current_codec_header), current_codec_header.size(),
                      legacy_codec_header);
    const auto features_record = legacy_v2.find("FEATURES 0\n");
    REQUIRE(features_record != std::string::npos);
    legacy_v2.erase(features_record, std::string("FEATURES 0\n").size());
    auto legacy_v1 = [&]() {
        std::string value = legacy_v2;
        const auto legacy_v1_header = std::string("CARTOGRAPHER_SEQUENCE_GENOME 1");
        value.replace(value.find(legacy_codec_header), legacy_codec_header.size(),
                      legacy_v1_header);
        return value;
    }();
    const auto variant_line_start = legacy_v1.find("VARIANT ");
    const auto variant_line_end = legacy_v1.find('\n', variant_line_start);
    REQUIRE(variant_line_start != std::string::npos);
    REQUIRE(variant_line_end != std::string::npos);
    const auto legacy_source_suffix = std::string(" \"\"");
    REQUIRE(legacy_v1.substr(variant_line_end - legacy_source_suffix.size(),
                             legacy_source_suffix.size()) == legacy_source_suffix);
    legacy_v1.erase(variant_line_end - legacy_source_suffix.size(), legacy_source_suffix.size());
    const auto legacy_restored = SequenceGenome::deserialize(legacy_v1);
    REQUIRE(legacy_restored);
    REQUIRE(legacy_restored.value().variants().at(VariantId{7U}).source_identifier.empty());
    const auto legacy_v2_restored = SequenceGenome::deserialize(legacy_v2);
    REQUIRE(legacy_v2_restored);
    REQUIRE(legacy_v2_restored.value().variants().at(VariantId{7U}).source_identifier.empty());

    SequenceGenome digest_only;
    digest_only.schema = "cartographer.sequence-genome.v1";
    digest_only.assembly = "fixture-assembly";
    REQUIRE(digest_only.insert_contig(SequenceContig{
        ContigId{1U}, "digest-only", "", carto::assets::Sha256Digest::from_hex(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").value(), provenance}));
    REQUIRE(digest_only.validate());
    REQUIRE(!digest_only.export_fasta());

    SequenceGenome invalid_base;
    invalid_base.schema = "cartographer.sequence-genome.v1";
    invalid_base.assembly = "fixture-assembly";
    REQUIRE(!invalid_base.insert_contig(SequenceContig{
        ContigId{1U}, "bad", "ACGTx", std::nullopt, provenance}));

    SequenceGenome mismatch;
    mismatch.schema = "cartographer.sequence-genome.v1";
    mismatch.assembly = "fixture-assembly";
    REQUIRE(mismatch.insert_contig(SequenceContig{
        ContigId{1U}, "chr-fixture", "AACGTNRY", std::nullopt, provenance}));
    REQUIRE(mismatch.insert_variant(SequenceVariant{
        VariantId{1U}, ContigId{1U}, 1U, "AA", "TT", "substitution", provenance, ""}));
    REQUIRE(!mismatch.validate());
    REQUIRE(!SequenceGenome::deserialize(mismatch.serialize()));

    SequenceGenome empty_contig;
    empty_contig.schema = "cartographer.sequence-genome.v1";
    empty_contig.assembly = "fixture-assembly";
    REQUIRE(!empty_contig.insert_contig(SequenceContig{
        ContigId{1U}, "empty", "", std::nullopt, provenance}));
    REQUIRE(!SequenceGenome::deserialize(
        "CARTOGRAPHER_SEQUENCE_GENOME 999\nEND\n"));
}

void sequence_genome_gff3_exchange_is_reference_bound_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const auto source_digest = carto::assets::Sha256Digest::from_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    REQUIRE(source_digest);

    SequenceGenome reference;
    reference.schema = "cartographer.sequence-genome.v3";
    reference.assembly = "fixture-assembly";
    reference.sample_reference = "sample://fixture";
    reference.source_digest = source_digest.value();
    REQUIRE(reference.insert_contig(SequenceContig{
        ContigId{1U}, "chr-fixture", "AACGTNRY", std::nullopt, provenance}));

    auto annotated = reference;
    REQUIRE(annotated.insert_feature(SequenceFeature{
        SequenceFeatureId{1U}, ContigId{1U}, 1U, 4U, "gene", "fixture", 0.75, "+", std::nullopt,
        "gene-1", {{"Name", "alpha beta"}, {"Parent", "root"}}, provenance}));
    REQUIRE(annotated.insert_feature(SequenceFeature{
        SequenceFeatureId{2U}, ContigId{1U}, 4U, 8U, "CDS", "fixture", std::nullopt, "-",
        std::uint8_t{0}, "", {{"Note", "encoded;value"}}, provenance}));
    REQUIRE(annotated.validate());

    const auto exported = annotated.export_gff3();
    REQUIRE(exported);
    REQUIRE(exported.value().text.find("##gff-version 3\n") == 0U);
    REQUIRE(exported.value().text.find(
                "##cartographer.coordinate_system=gff3-1-based-inclusive\n") != std::string::npos);
    REQUIRE(exported.value().text.find("CARTOGRAPHER_ID=1;ID=gene-1;Name=alpha%20beta;Parent=root") !=
            std::string::npos);
    REQUIRE(exported.value().text.find("Note=encoded%3Bvalue") != std::string::npos);
    REQUIRE(exported.value().feature_loss_notes.size() == 3U);

    const auto imported = SequenceGenome::import_gff3(
        exported.value().text, reference, provenance);
    REQUIRE(imported);
    REQUIRE(imported.value().features().size() == 2U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).start == 1U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).end == 4U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).source_identifier == "gene-1");
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).attributes.at(0U).value ==
            "alpha beta");
    REQUIRE(imported.value().features().at(SequenceFeatureId{2U}).phase == std::uint8_t{0});
    REQUIRE(imported.value().export_gff3().value().text == exported.value().text);
    REQUIRE(SequenceGenome::deserialize(imported.value().serialize()).value().features().size() == 2U);

    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };
    REQUIRE(!SequenceGenome::import_gff3(exported.value().text, reference, Provenance{}));

    auto wrong_reference = reference;
    wrong_reference.source_digest = carto::assets::Sha256Digest::from_hex(
        "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd").value();
    REQUIRE(!SequenceGenome::import_gff3(exported.value().text, wrong_reference, provenance));

    auto occupied = reference;
    REQUIRE(occupied.insert_feature(SequenceFeature{
        SequenceFeatureId{99U}, ContigId{1U}, 0U, 1U, "existing", "fixture", std::nullopt, ".",
        std::nullopt, "existing", {}, provenance}));
    REQUIRE(!SequenceGenome::import_gff3(exported.value().text, occupied, provenance));

    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "##gff-version 3\n", "##gff-version 3\n##source=external\n"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text,
                     "##cartographer.coordinate_system=gff3-1-based-inclusive",
                     "##cartographer.coordinate_system=zero-based"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "chr-fixture\tfixture", "chr-missing\tfixture"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "\t2\t4\t0.75", "\t0\t4\t0.75"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "\t0.75\t+", "\tnan\t+"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "CARTOGRAPHER_ID=2", "CARTOGRAPHER_ID=1"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "alpha%20beta", "alpha%ZZbeta"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text, "\tCARTOGRAPHER_ID=1;", "\tCARTOGRAPHER_ID=1;Name=duplicate;"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gff3(
        replace_once(exported.value().text,
                     "chr-fixture\tfixture\tgene\t2\t4\t0.75\t+\t.\t",
                     "chr-fixture\tfixture\tgene\t2\t4\t0.75\t+\t.\textra\t"),
        reference, provenance));
}

void sequence_genome_bed6_exchange_is_reference_bound_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const auto source_digest = carto::assets::Sha256Digest::from_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    REQUIRE(source_digest);

    SequenceGenome reference;
    reference.schema = "cartographer.sequence-genome.v3";
    reference.assembly = "fixture-assembly";
    reference.sample_reference = "sample://fixture";
    reference.source_digest = source_digest.value();
    REQUIRE(reference.insert_contig(SequenceContig{
        ContigId{1U}, "chr-fixture", "AACGTNRY", std::nullopt, provenance}));

    auto annotated = reference;
    REQUIRE(annotated.insert_feature(SequenceFeature{
        SequenceFeatureId{1U}, ContigId{1U}, 1U, 4U, "gene", "fixture", 750.0, "+", std::nullopt,
        "gene-1", {{"Name", "alpha"}}, provenance}));
    REQUIRE(annotated.insert_feature(SequenceFeature{
        SequenceFeatureId{2U}, ContigId{1U}, 4U, 8U, "CDS", "fixture", 0.75, "-", std::uint8_t{0},
        "", {{"Note", "not carried"}}, provenance}));
    REQUIRE(annotated.validate());

    const auto exported = annotated.export_bed6();
    REQUIRE(exported);
    REQUIRE(exported.value().text.find("#cartographer.bed_version=6\n") == 0U);
    REQUIRE(exported.value().text.find(
                "#cartographer.coordinate_system=bed-0-based-half-open\n") != std::string::npos);
    REQUIRE(exported.value().text.find("chr-fixture\t1\t4\tcartographer:1:67656e652d31\t750\t+\n") !=
            std::string::npos);
    REQUIRE(exported.value().text.find("chr-fixture\t4\t8\tcartographer:2:\t1\t-\n") !=
            std::string::npos);
    REQUIRE(exported.value().feature_loss_notes.size() == 4U);

    const auto imported = SequenceGenome::import_bed6(
        exported.value().text, reference, provenance);
    REQUIRE(imported);
    REQUIRE(imported.value().features().size() == 2U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).start == 1U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).source_identifier == "gene-1");
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).score == 750.0);
    REQUIRE(imported.value().features().at(SequenceFeatureId{2U}).score == 1.0);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).type == "bed6");
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).attributes.empty());
    REQUIRE(imported.value().export_bed6().value().text == exported.value().text);
    REQUIRE(SequenceGenome::deserialize(imported.value().serialize()).value().features().size() == 2U);

    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };
    REQUIRE(!SequenceGenome::import_bed6(exported.value().text, reference, Provenance{}));

    auto wrong_reference = reference;
    wrong_reference.source_digest = carto::assets::Sha256Digest::from_hex(
        "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd").value();
    REQUIRE(!SequenceGenome::import_bed6(exported.value().text, wrong_reference, provenance));

    auto occupied = reference;
    REQUIRE(occupied.insert_feature(SequenceFeature{
        SequenceFeatureId{99U}, ContigId{1U}, 0U, 1U, "existing", "fixture", std::nullopt, ".",
        std::nullopt, "existing", {}, provenance}));
    REQUIRE(!SequenceGenome::import_bed6(exported.value().text, occupied, provenance));

    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "#cartographer.bed_version=6\n",
                     "#cartographer.bed_version=6\n#track name=external\n"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text,
                     "#cartographer.coordinate_system=bed-0-based-half-open",
                     "#cartographer.coordinate_system=gff3-1-based-inclusive"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "chr-fixture\t1\t4", "chr-missing\t1\t4"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "\t1\t4\tcartographer", "\t4\t4\tcartographer"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "\t1\t4\tcartographer", "\t1\t9\tcartographer"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "\t750\t+", "\t1001\t+"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "\t750\t+", "\t750\t?"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "cartographer:1:67656e652d31", "cartographer:bad:"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "cartographer:2:", "cartographer:1:"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_bed6(
        replace_once(exported.value().text, "\t1\t4\tcartographer:1:67656e652d31\t750\t+",
                     "\t1\t4\textra\tcartographer:1:67656e652d31\t750\t+"),
        reference, provenance));

    auto unknown_strand = annotated;
    REQUIRE(unknown_strand.insert_feature(SequenceFeature{
        SequenceFeatureId{3U}, ContigId{1U}, 0U, 1U, "unknown", "fixture", std::nullopt, "?",
        std::nullopt, "unknown", {}, provenance}));
    REQUIRE(!unknown_strand.export_bed6());
}

void sequence_genome_gtf_exchange_is_reference_bound_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const auto source_digest = carto::assets::Sha256Digest::from_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    REQUIRE(source_digest);

    SequenceGenome reference;
    reference.schema = "cartographer.sequence-genome.v3";
    reference.assembly = "fixture-assembly";
    reference.sample_reference = "sample://fixture";
    reference.source_digest = source_digest.value();
    REQUIRE(reference.insert_contig(SequenceContig{
        ContigId{1U}, "chr-fixture", "AACGTNRY", std::nullopt, provenance}));

    auto annotated = reference;
    REQUIRE(annotated.insert_feature(SequenceFeature{
        SequenceFeatureId{1U}, ContigId{1U}, 1U, 4U, "exon", "fixture", 0.5, "+", std::uint8_t{1},
        "gene-1", {{"gene_id", "gene-1"}, {"Note", "quote \" slash \\"}}, provenance}));
    REQUIRE(annotated.insert_feature(SequenceFeature{
        SequenceFeatureId{2U}, ContigId{1U}, 4U, 8U, "CDS", "fixture", std::nullopt, "-",
        std::nullopt, "", {{"transcript_id", "tx-1"}}, provenance}));
    REQUIRE(annotated.validate());

    const auto exported = annotated.export_gtf();
    REQUIRE(exported);
    REQUIRE(exported.value().text.find("#!cartographer.gtf_version=2.2\n") == 0U);
    REQUIRE(exported.value().text.find(
                "#!cartographer.coordinate_system=gtf-1-based-inclusive\n") != std::string::npos);
    REQUIRE(exported.value().text.find("CARTOGRAPHER_ID \"1\";") != std::string::npos);
    REQUIRE(exported.value().text.find("cartographer_source_id \"gene-1\";") != std::string::npos);
    REQUIRE(exported.value().feature_loss_notes.size() == 3U);

    const auto imported = SequenceGenome::import_gtf(
        exported.value().text, reference, provenance);
    REQUIRE(imported);
    REQUIRE(imported.value().features().size() == 2U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).start == 1U);
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).phase == std::uint8_t{1});
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).source_identifier == "gene-1");
    REQUIRE(imported.value().features().at(SequenceFeatureId{1U}).attributes.at(1U).value ==
            "quote \" slash \\");
    REQUIRE(imported.value().export_gtf().value().text == exported.value().text);
    REQUIRE(SequenceGenome::deserialize(imported.value().serialize()).value().features().size() == 2U);

    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };
    REQUIRE(!SequenceGenome::import_gtf(exported.value().text, reference, Provenance{}));

    auto wrong_reference = reference;
    wrong_reference.source_digest = carto::assets::Sha256Digest::from_hex(
        "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd").value();
    REQUIRE(!SequenceGenome::import_gtf(exported.value().text, wrong_reference, provenance));

    auto occupied = reference;
    REQUIRE(occupied.insert_feature(SequenceFeature{
        SequenceFeatureId{99U}, ContigId{1U}, 0U, 1U, "existing", "fixture", std::nullopt, ".",
        std::nullopt, "existing", {}, provenance}));
    REQUIRE(!SequenceGenome::import_gtf(exported.value().text, occupied, provenance));

    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "#!cartographer.gtf_version=2.2\n",
                     "#!cartographer.gtf_version=2.2\n#!genome-build=external\n"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text,
                     "#!cartographer.coordinate_system=gtf-1-based-inclusive",
                     "#!cartographer.coordinate_system=gff3-1-based-inclusive"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "chr-fixture\tfixture", "chr-missing\tfixture"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "\t0.5\t+", "\tnan\t+"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "gene_id \"gene-1\";", "gene_id gene-1;"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "gene_id \"gene-1\";", "gene_id \"gene-1\""),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "CARTOGRAPHER_ID \"2\";", "CARTOGRAPHER_ID \"1\";"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text,
                     "chr-fixture\tfixture\texon\t2\t4\t0.5\t+\t1\t",
                     "chr-fixture\tfixture\texon\t2\t4\t0.5\t+\t1\textra\t"),
        reference, provenance));
    REQUIRE(!SequenceGenome::import_gtf(
        replace_once(exported.value().text, "\\\" slash", "\\q slash"),
        reference, provenance));
}

void anatomy_ontology_table_exchange_is_strict_and_semantic_only() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    AnatomyModel source;
    source.schema = "cartographer.anatomy.v3";
    REQUIRE(source.insert_term(AnatomyTerm{
        AnatomyTermId{1U}, "UBERON", "UBERON:0000000", "organism", "organism",
        std::nullopt, "root definition", provenance, "2026-01", "uberon-release"}));
    REQUIRE(source.insert_term(AnatomyTerm{
        AnatomyTermId{2U}, "UBERON", "UBERON:0002107", "liver", "organ",
        AnatomyTermId{1U}, "liver definition", provenance, "2026-01", "uberon-release"}));
    REQUIRE(source.validate());

    const auto exported = source.export_ontology_table();
    REQUIRE(exported);
    REQUIRE(exported.value().text.find(
                "#cartographer.ontology_table_version=1\n") == 0U);
    REQUIRE(exported.value().text.find(
                "#cartographer.term_identity=cartographer-anatomy-term-v1\n") !=
            std::string::npos);
    REQUIRE(exported.value().text.find("UBERON") == std::string::npos);
    REQUIRE(exported.value().feature_loss_notes.size() == 3U);

    AnatomyModel reference;
    reference.schema = "cartographer.anatomy.v3";
    const auto imported = AnatomyModel::import_ontology_table(
        exported.value().text, reference, provenance);
    REQUIRE(imported);
    REQUIRE(imported.value().terms().size() == 2U);
    REQUIRE(imported.value().terms().at(AnatomyTermId{2U}).parent_term == AnatomyTermId{1U});
    REQUIRE(imported.value().terms().at(AnatomyTermId{2U}).ontology_version == "2026-01");
    REQUIRE(imported.value().terms().at(AnatomyTermId{2U}).source_release == "uberon-release");
    REQUIRE(imported.value().export_ontology_table().value().text == exported.value().text);
    REQUIRE(AnatomyModel::deserialize(imported.value().serialize()).value().terms().size() == 2U);

    const auto replace_once = [](std::string value, std::string_view from,
                                 std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    };
    REQUIRE(!AnatomyModel::import_ontology_table(exported.value().text, reference, Provenance{}));
    AnatomyModel wrong_schema;
    wrong_schema.schema = "cartographer.anatomy.other";
    REQUIRE(!AnatomyModel::import_ontology_table(
        exported.value().text, wrong_schema, provenance));
    AnatomyModel occupied;
    occupied.schema = "cartographer.anatomy.v3";
    REQUIRE(occupied.insert_term(AnatomyTerm{
        AnatomyTermId{99U}, "fixture", "FIX:99", "existing", "organ", std::nullopt,
        "", provenance, "", ""}));
    REQUIRE(!AnatomyModel::import_ontology_table(exported.value().text, occupied, provenance));
    REQUIRE(!AnatomyModel::import_ontology_table(
        replace_once(exported.value().text, "#cartographer.term_identity=cartographer-anatomy-term-v1",
                     "#cartographer.term_identity=external"),
        reference, provenance));
    REQUIRE(!AnatomyModel::import_ontology_table(
        replace_once(exported.value().text, "#cartographer.schema_hex=636172746f677261706865722e616e61746f6d792e7633",
                     "#cartographer.schema_hex=666f72676564"),
        reference, provenance));
    REQUIRE(!AnatomyModel::import_ontology_table(
        replace_once(exported.value().text, "636172746f677261706865722e616e61746f6d79",
                     "zz"),
        reference, provenance));
    REQUIRE(!AnatomyModel::import_ontology_table(
        replace_once(exported.value().text, "1\t0\t554245524f4e\t554245524f4e3a30303030303030",
                     "2\t0\t554245524f4e\t554245524f4e3a30303030303030"),
        reference, provenance));
    REQUIRE(!AnatomyModel::import_ontology_table(
        replace_once(exported.value().text, "1\t0\t554245524f4e\t554245524f4e3a30303030303030",
                     "1\t99\t554245524f4e\t554245524f4e3a30303030303030"),
        reference, provenance));
    REQUIRE(!AnatomyModel::import_ontology_table(
        replace_once(exported.value().text, "\t554245524f4e3a30303030303030\t",
                     "\t554245524f4e3a30303030303030\textra\t"),
        reference, provenance));
}

void anatomy_partonomy_round_trip_and_reference_checks() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();
    AnatomyModel anatomy;
    anatomy.schema = "cartographer.anatomy.v1";
    REQUIRE(anatomy.insert_term(AnatomyTerm{
        AnatomyTermId{1U}, "fixture", "ORG:1", "fixture organ", "organ", std::nullopt,
        "authored organ term", provenance, "uberon-2026-01", "fixture-release-1"}));
    REQUIRE(anatomy.insert_term(AnatomyTerm{
        AnatomyTermId{2U}, "fixture", "TIS:1", "fixture tissue", "tissue", AnatomyTermId{1U},
        "authored tissue term", provenance, "", ""}));
    REQUIRE(anatomy.insert_part(AnatomyPart{
        AnatomyPartId{1U}, StructureId{1U}, AnatomyTermId{1U}, std::nullopt, "organ", provenance}));
    REQUIRE(anatomy.insert_part(AnatomyPart{
        AnatomyPartId{2U}, StructureId{2U}, AnatomyTermId{2U}, AnatomyPartId{1U}, "wall", provenance}));
    REQUIRE(anatomy.insert_tissue_region(TissueRegion{
        TissueRegionId{1U}, "muscle", StructureId{2U}, RegionId{2U},
        "density=1;elasticity=0.5", "stage:1", "stage:1",
        GeometryBinding{"surface", "object:1"}, provenance}));
    REQUIRE(anatomy.insert_relation(AnatomyRelation{
        AnatomyRelationId{1U}, AnatomyPartId{1U}, AnatomyPartId{2U}, "contains", provenance}));
    REQUIRE(model.set_anatomy(std::move(anatomy)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("ANATOMY 3") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().anatomy().has_value());
    REQUIRE(restored.value().anatomy()->terms().size() == 2U);
    REQUIRE(restored.value().anatomy()->parts().at(AnatomyPartId{2U}).parent_part == AnatomyPartId{1U});
    REQUIRE(restored.value().anatomy()->tissue_regions().at(TissueRegionId{1U}).structure ==
            StructureId{2U});
    REQUIRE(restored.value().anatomy()->terms().at(AnatomyTermId{1U}).ontology_version ==
            "uberon-2026-01");
    REQUIRE(restored.value().anatomy()->terms().at(AnatomyTermId{1U}).source_release ==
            "fixture-release-1");
    REQUIRE(restored.value().serialize() == encoded);

    std::string legacy_v2 = restored.value().anatomy()->serialize();
    const auto header = legacy_v2.find("CARTOGRAPHER_ANATOMY 3");
    REQUIRE(header != std::string::npos);
    legacy_v2.replace(header, std::string("CARTOGRAPHER_ANATOMY 3").size(),
                      "CARTOGRAPHER_ANATOMY 2");
    const auto metadata = legacy_v2.find("\"uberon-2026-01\" \"fixture-release-1\"");
    REQUIRE(metadata != std::string::npos);
    legacy_v2.replace(metadata, std::string("\"uberon-2026-01\" \"fixture-release-1\"").size(),
                      "\"\" \"\"");
    const std::string empty_metadata = " \"\" \"\"\n";
    std::size_t empty_metadata_offset = legacy_v2.find(empty_metadata);
    while (empty_metadata_offset != std::string::npos) {
        legacy_v2.replace(empty_metadata_offset, empty_metadata.size(), "\n");
        empty_metadata_offset = legacy_v2.find(empty_metadata);
    }
    const auto legacy_v2_restored = AnatomyModel::deserialize(legacy_v2);
    REQUIRE(legacy_v2_restored);
    REQUIRE(legacy_v2_restored.value().tissue_regions().size() == 1U);
    REQUIRE(legacy_v2_restored.value().terms().at(AnatomyTermId{1U}).ontology_version.empty());

    std::string legacy_v1 = legacy_v2;
    const auto legacy_v1_header = legacy_v1.find("CARTOGRAPHER_ANATOMY 2");
    REQUIRE(legacy_v1_header != std::string::npos);
    legacy_v1.replace(legacy_v1_header, std::string("CARTOGRAPHER_ANATOMY 2").size(),
                      "CARTOGRAPHER_ANATOMY 1");
    const auto tissue_section = legacy_v1.find("TISSUE_REGIONS 1\n");
    REQUIRE(tissue_section != std::string::npos);
    const auto relations_section = legacy_v1.find("RELATIONS 1\n", tissue_section);
    REQUIRE(relations_section != std::string::npos);
    legacy_v1.erase(tissue_section, relations_section - tissue_section);
    const auto legacy_v1_restored = AnatomyModel::deserialize(legacy_v1);
    REQUIRE(legacy_v1_restored);
    REQUIRE(legacy_v1_restored.value().tissue_regions().empty());
    REQUIRE(legacy_v1_restored.value().terms().at(AnatomyTermId{1U}).source_release.empty());

    std::string missing_v3_metadata = restored.value().anatomy()->serialize();
    const auto missing_metadata = missing_v3_metadata.find(
        "\"uberon-2026-01\" \"fixture-release-1\"");
    REQUIRE(missing_metadata != std::string::npos);
    missing_v3_metadata.replace(
        missing_metadata, std::string("\"uberon-2026-01\" \"fixture-release-1\"").size(),
        "\"uberon-2026-01\"");
    REQUIRE(!AnatomyModel::deserialize(missing_v3_metadata));

    AnatomyModel invalid_tissue;
    invalid_tissue.schema = "cartographer.anatomy.v1";
    REQUIRE(!invalid_tissue.insert_tissue_region(TissueRegion{
        TissueRegionId{1U}, "", std::nullopt, std::nullopt, "", "", "",
        GeometryBinding{}, provenance}));

    AnatomyModel cycle;
    cycle.schema = "cartographer.anatomy.v1";
    REQUIRE(cycle.insert_term(AnatomyTerm{
        AnatomyTermId{1U}, "fixture", "A", "a", "organ", AnatomyTermId{2U}, "", provenance,
        "", ""}));
    REQUIRE(cycle.insert_term(AnatomyTerm{
        AnatomyTermId{2U}, "fixture", "B", "b", "tissue", AnatomyTermId{1U}, "", provenance,
        "", ""}));
    REQUIRE(!cycle.validate());

    AnatomyModel orphan;
    orphan.schema = "cartographer.anatomy.v1";
    REQUIRE(orphan.insert_term(AnatomyTerm{
        AnatomyTermId{1U}, "fixture", "A", "a", "organ", std::nullopt, "", provenance, "", ""}));
    REQUIRE(orphan.insert_part(AnatomyPart{
        AnatomyPartId{1U}, StructureId{999U}, AnatomyTermId{1U}, std::nullopt, "orphan", provenance}));
    auto orphan_model = make_model();
    REQUIRE(orphan_model.set_anatomy(std::move(orphan)));
    REQUIRE(!orphan_model.validate());
    REQUIRE(!AnatomyModel::deserialize("CARTOGRAPHER_ANATOMY 999\nEND\n"));
}

void field_visualization_round_trip_and_reference_checks() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();
    FieldVisualizationModel visualizations;
    visualizations.schema = "cartographer.field-visualizations.v1";
    REQUIRE(visualizations.insert_visualization(FieldVisualization{
        VisualizationId{1U}, FieldId{1U}, RegionId{2U}, "scalar", "viridis", 0.0, 1.0,
        "stage:1", true, provenance}));
    REQUIRE(model.set_field_visualizations(std::move(visualizations)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("FIELD_VISUALIZATIONS 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().field_visualizations().has_value());
    REQUIRE(restored.value().field_visualizations()->visualizations().at(VisualizationId{1U}).palette ==
            "viridis");
    REQUIRE(restored.value().serialize() == encoded);

    const FieldVisualization scalar_preview_source{
        VisualizationId{2U}, FieldId{1U}, RegionId{2U}, "scalar", "viridis", 0.0, 1.0,
        "stage:1", true, provenance};
    const std::vector<double> scalar_samples{-1.0, 0.25, 0.75, 2.0};
    const auto scalar_preview = scalar_preview_source.preview_samples(scalar_samples);
    REQUIRE(scalar_preview);
    REQUIRE(scalar_preview.value().validate());
    REQUIRE(scalar_preview.value().lower_bound == 0.0);
    REQUIRE(scalar_preview.value().upper_bound == 1.0);
    REQUIRE(scalar_preview.value().clipped_low == 1U);
    REQUIRE(scalar_preview.value().clipped_high == 1U);
    REQUIRE(scalar_preview.value().samples.size() == 4U);
    REQUIRE(scalar_preview.value().samples.at(0U).normalized_value == 0.0);
    REQUIRE(std::abs(scalar_preview.value().samples.at(1U).normalized_value - 0.25) < 1e-12);
    REQUIRE(std::abs(scalar_preview.value().samples.at(2U).normalized_value - 0.75) < 1e-12);
    REQUIRE(scalar_preview.value().samples.at(3U).normalized_value == 1.0);
    REQUIRE(scalar_preview.value().legend_ticks.size() == 5U);
    REQUIRE(scalar_preview.value().samples.at(0U).rgba[3] == 1.0);
    auto forged_preview = scalar_preview.value();
    forged_preview.samples.at(1U).normalized_value = 0.9;
    REQUIRE(!forged_preview.validate());

    const FieldVisualization vector_preview_source{
        VisualizationId{6U}, FieldId{1U}, std::nullopt, "vector", "viridis", 0.0, 2.0,
        "stage:1", false, provenance};
    const std::vector<FieldVectorSample> vector_samples{
        {{0.0, 0.0, 0.0}, {3.0, 4.0, 0.0}},
        {{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0}},
        {{2.0, 3.0, 4.0}, {0.0, 0.0, 2.0}},
    };
    const auto vector_preview = vector_preview_source.preview_vector_glyphs(vector_samples, 0.5);
    REQUIRE(vector_preview);
    REQUIRE(vector_preview.value().validate());
    REQUIRE(vector_preview.value().clipped_high == 1U);
    REQUIRE(vector_preview.value().zero_vectors == 1U);
    REQUIRE(vector_preview.value().glyphs.size() == 3U);
    REQUIRE(vector_preview.value().glyphs.at(0U).direction.x == 0.6);
    REQUIRE(vector_preview.value().glyphs.at(0U).direction.y == 0.8);
    REQUIRE(vector_preview.value().glyphs.at(0U).length == 2.5);
    REQUIRE(vector_preview.value().glyphs.at(1U).direction.x == 0.0);
    REQUIRE(vector_preview.value().glyphs.at(1U).length == 0.0);
    auto forged_vector_preview = vector_preview.value();
    forged_vector_preview.glyphs.at(2U).direction.x = 1.0;
    REQUIRE(!forged_vector_preview.validate());
    forged_vector_preview = vector_preview.value();
    forged_vector_preview.glyphs.at(0U).source_vector = {0.0, 5.0, 0.0};
    REQUIRE(!forged_vector_preview.validate());
    REQUIRE(!vector_preview_source.preview_vector_glyphs(vector_samples, 0.0));
    const FieldVectorSample non_finite_position{{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
                                                  {1.0, 0.0, 0.0}};
    REQUIRE(!vector_preview_source.preview_vector_glyphs(std::vector<FieldVectorSample>{non_finite_position}));

    const FieldVisualization auto_range_preview_source{
        VisualizationId{3U}, FieldId{1U}, std::nullopt, "scalar", "gray", std::nullopt,
        std::nullopt, "", false, provenance};
    const std::vector<double> auto_range_samples{-2.0, 0.0, 2.0};
    const auto auto_range_preview = auto_range_preview_source.preview_samples(auto_range_samples);
    REQUIRE(auto_range_preview);
    REQUIRE(auto_range_preview.value().lower_bound == -2.0);
    REQUIRE(auto_range_preview.value().upper_bound == 2.0);
    REQUIRE(auto_range_preview.value().samples.at(1U).rgba[0] == 0.5);

    const FieldVisualization unsupported_palette{
        VisualizationId{4U}, FieldId{1U}, std::nullopt, "scalar", "magma", 0.0, 1.0,
        "", false, provenance};
    REQUIRE(!unsupported_palette.preview_samples(std::vector<double>{0.5}));
    const FieldVisualization categorical_mode{
        VisualizationId{5U}, FieldId{1U}, std::nullopt, "categorical", "gray", 0.0, 1.0,
        "", false, provenance};
    REQUIRE(!categorical_mode.preview_samples(std::vector<double>{0.5}));
    REQUIRE(!scalar_preview_source.preview_samples(
        std::vector<double>{0.0, std::numeric_limits<double>::quiet_NaN()}));

    FieldVisualizationModel inverted;
    inverted.schema = "cartographer.field-visualizations.v1";
    REQUIRE(!inverted.insert_visualization(FieldVisualization{
        VisualizationId{1U}, FieldId{1U}, std::nullopt, "scalar", "gray", 2.0, 1.0,
        "", false, provenance}));

    FieldVisualizationModel orphan;
    orphan.schema = "cartographer.field-visualizations.v1";
    REQUIRE(orphan.insert_visualization(FieldVisualization{
        VisualizationId{1U}, FieldId{999U}, std::nullopt, "scalar", "gray", std::nullopt,
        std::nullopt, "", false, provenance}));
    auto orphan_model = make_model();
    REQUIRE(orphan_model.set_field_visualizations(std::move(orphan)));
    REQUIRE(!orphan_model.validate());
}

void field_slice_and_contour_previews_are_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const FieldVisualization source{
        VisualizationId{7U}, FieldId{1U}, std::nullopt, "scalar", "viridis", 0.0, 1.0,
        "stage:1", true, provenance};
    const std::vector<double> grid{
        0.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 0.0};
    const auto slice = source.preview_slice(
        grid, 3U, 3U, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0},
        {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    REQUIRE(slice);
    REQUIRE(slice.value().validate());
    REQUIRE(slice.value().samples.size() == 9U);
    REQUIRE(slice.value().samples.at(4U).source_value == 1.0);
    REQUIRE(slice.value().samples.at(4U).normalized_value == 1.0);
    const auto repeated_slice = source.preview_slice(
        grid, 3U, 3U, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0},
        {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    REQUIRE(repeated_slice);
    REQUIRE(repeated_slice.value().samples.size() == slice.value().samples.size());
    REQUIRE(repeated_slice.value().samples.at(4U).rgba == slice.value().samples.at(4U).rgba);
    REQUIRE(!source.preview_slice(
        grid, 3U, 2U, {0.0, 0.0, 0.0}, {0.0, 0.0, 2.0},
        {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}));
    REQUIRE(!source.preview_slice(
        std::vector<double>{0.0, 1.0}, 3U, 3U, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0},
        {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}));
    auto forged_slice = slice.value();
    forged_slice.horizontal_axis = {0.0, 1.0, 0.0};
    REQUIRE(!forged_slice.validate());

    const std::vector<double> levels{0.5};
    const auto contours = source.preview_contours(grid, 3U, 3U, levels);
    REQUIRE(contours);
    REQUIRE(contours.value().validate());
    REQUIRE(contours.value().segments.size() == 4U);
    REQUIRE(contours.value().segments.at(0U).level == 0.5);
    const auto repeated_contours = source.preview_contours(grid, 3U, 3U, levels);
    REQUIRE(repeated_contours);
    REQUIRE(repeated_contours.value().segments.size() == contours.value().segments.size());
    REQUIRE(repeated_contours.value().segments.at(0U).start.x ==
            contours.value().segments.at(0U).start.x);
    REQUIRE(repeated_contours.value().segments.at(0U).end.y ==
            contours.value().segments.at(0U).end.y);
    auto forged_contours = contours.value();
    forged_contours.segments.at(0U).rgba[0] = 0.0;
    REQUIRE(!forged_contours.validate());
    forged_contours = contours.value();
    forged_contours.segments.at(0U).level = 0.25;
    REQUIRE(!forged_contours.validate());
    REQUIRE(!source.preview_contours(grid, 3U, 3U, std::vector<double>{0.75, 0.25}));
    REQUIRE(!source.preview_contours(
        std::vector<double>{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
        2U, 2U, levels));
}

void field_isosurface_previews_are_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const FieldVisualization source{
        VisualizationId{13U}, FieldId{1U}, std::nullopt, "scalar", "viridis", 0.0, 1.0,
        "stage:1", true, provenance};
    const std::vector<double> volume{
        1.0, 0.0,
        0.0, 0.0,
        0.0, 0.0,
        0.0, 0.0};
    const auto preview = source.preview_isosurface(volume, 2U, 2U, 2U, 0.5);
    REQUIRE(preview);
    REQUIRE(preview.value().validate());
    REQUIRE(preview.value().triangles.size() == 6U);
    REQUIRE(preview.value().triangles.at(0U).level == 0.5);
    REQUIRE(preview.value().triangles.at(0U).rgba[3] == 1.0);
    for (const auto& triangle : preview.value().triangles) {
        REQUIRE(triangle.a.x >= 0.0);
        REQUIRE(triangle.a.x <= 1.0);
        REQUIRE(triangle.a.y >= 0.0);
        REQUIRE(triangle.a.y <= 1.0);
        REQUIRE(triangle.a.z >= 0.0);
        REQUIRE(triangle.a.z <= 1.0);
    }

    const auto repeated = source.preview_isosurface(volume, 2U, 2U, 2U, 0.5);
    REQUIRE(repeated);
    REQUIRE(repeated.value().triangles.size() == preview.value().triangles.size());
    REQUIRE(repeated.value().triangles.at(0U).a.x == preview.value().triangles.at(0U).a.x);
    REQUIRE(repeated.value().triangles.at(0U).b.y == preview.value().triangles.at(0U).b.y);
    REQUIRE(repeated.value().triangles.at(5U).c.z == preview.value().triangles.at(5U).c.z);

    auto forged = preview.value();
    forged.triangles.at(0U).rgba[0] = 0.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.triangles.at(0U).c = forged.triangles.at(0U).a;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.triangles.at(0U).level = 0.25;
    REQUIRE(!forged.validate());

    const auto flat = source.preview_isosurface(
        std::vector<double>(8U, 0.5), 2U, 2U, 2U, 0.5);
    REQUIRE(flat);
    REQUIRE(flat.value().triangles.empty());
    REQUIRE(flat.value().validate());

    REQUIRE(!source.preview_isosurface(volume, 1U, 2U, 2U, 0.5));
    REQUIRE(!source.preview_isosurface(std::vector<double>(7U, 0.0), 2U, 2U, 2U, 0.5));
    REQUIRE(!source.preview_isosurface(
        std::vector<double>{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0,
                            0.0, 0.0, 0.0, 0.0},
        2U, 2U, 2U, 0.5));
    REQUIRE(!source.preview_isosurface(volume, 2U, 2U, 2U, 2.0));

    const FieldVisualization vector_source{
        VisualizationId{14U}, FieldId{1U}, std::nullopt, "vector", "gray", 0.0, 1.0,
        "", false, provenance};
    REQUIRE(!vector_source.preview_isosurface(volume, 2U, 2U, 2U, 0.5));
}

void field_volume_previews_are_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const FieldVisualization source{
        VisualizationId{15U}, FieldId{1U}, std::nullopt, "scalar", "viridis", 0.0, 1.0,
        "stage:1", true, provenance};
    const std::vector<double> volume{
        1.0, 0.0,
        0.0, 0.0,
        0.0, 0.0,
        0.0, 0.0};
    const auto preview = source.preview_volume(volume, 2U, 2U, 2U, 2U, 2U, 0.5);
    REQUIRE(preview);
    REQUIRE(preview.value().validate());
    REQUIRE(preview.value().pixels.size() == 4U);
    REQUIRE(preview.value().pixels.at(0U).source_lower == 0.0);
    REQUIRE(preview.value().pixels.at(0U).source_upper == 1.0);
    REQUIRE(preview.value().pixels.at(0U).sample_count == 2U);
    const std::vector<double> expected_ray{1.0, 0.0};
    REQUIRE(preview.value().pixels.at(0U).source_values == expected_ray);
    REQUIRE(preview.value().pixels.at(0U).rgba[3] == 0.5);
    REQUIRE(preview.value().pixels.at(1U).rgba[3] == 0.0);
    REQUIRE(preview.value().clipped_low == 0U);
    REQUIRE(preview.value().clipped_high == 0U);

    const auto repeated = source.preview_volume(volume, 2U, 2U, 2U, 2U, 2U, 0.5);
    REQUIRE(repeated);
    REQUIRE(repeated.value().pixels.size() == preview.value().pixels.size());
    REQUIRE(repeated.value().pixels.at(0U).rgba == preview.value().pixels.at(0U).rgba);
    REQUIRE(repeated.value().pixels.at(3U).source_upper ==
            preview.value().pixels.at(3U).source_upper);

    auto forged = preview.value();
    forged.pixels.at(0U).rgba[0] = 1.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.pixels.at(0U).sample_count = 1U;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.pixels.at(0U).source_lower = 2.0;
    forged.pixels.at(0U).source_upper = 1.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.pixels.at(0U).source_values.at(0U) = 0.0;
    REQUIRE(!forged.validate());

    const auto flat = source.preview_volume(
        std::vector<double>(8U, 0.5), 2U, 2U, 2U, 2U, 2U, 0.5);
    REQUIRE(flat);
    REQUIRE(flat.value().pixels.at(0U).rgba[3] == 0.4375);
    REQUIRE(flat.value().validate());

    REQUIRE(!source.preview_volume(volume, 1U, 2U, 2U, 2U, 2U, 0.5));
    REQUIRE(!source.preview_volume(volume, 2U, 2U, 2U, 1U, 2U, 0.5));
    REQUIRE(!source.preview_volume(std::vector<double>(7U, 0.0), 2U, 2U, 2U, 2U, 2U, 0.5));
    REQUIRE(!source.preview_volume(volume, 2U, 2U, 2U, 2U, 2U, -0.1));
    REQUIRE(!source.preview_volume(volume, 2U, 2U, 2U, 2U, 2U, 1.1));
    REQUIRE(!source.preview_volume(
        std::vector<double>{}, 2U, 2U, 1000U, 100U, 100U, 0.5));
    REQUIRE(!source.preview_volume(
        std::vector<double>{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0,
                            0.0, 0.0, 0.0, 0.0},
        2U, 2U, 2U, 2U, 2U, 0.5));

    const FieldVisualization vector_source{
        VisualizationId{16U}, FieldId{1U}, std::nullopt, "vector", "gray", 0.0, 1.0,
        "", false, provenance};
    REQUIRE(!vector_source.preview_volume(volume, 2U, 2U, 2U, 2U, 2U, 0.5));
}

void field_probe_previews_are_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const FieldVisualization source{
        VisualizationId{8U}, FieldId{1U}, std::nullopt, "scalar", "viridis", 0.0, 1.0,
        "stage:1", true, provenance};

    const auto high = source.preview_probe(ProbeId{4U}, {1.0, 2.0, 3.0}, 2.0);
    REQUIRE(high);
    REQUIRE(high.value().validate());
    REQUIRE(high.value().visualization == VisualizationId{8U});
    REQUIRE(high.value().field == FieldId{1U});
    REQUIRE(high.value().probe == ProbeId{4U});
    REQUIRE(high.value().position.x == 1.0);
    REQUIRE(high.value().clipped_high);
    REQUIRE(!high.value().clipped_low);
    REQUIRE(high.value().normalized_value == 1.0);
    REQUIRE(high.value().rgba[3] == 1.0);

    const auto repeated = source.preview_probe(ProbeId{4U}, {1.0, 2.0, 3.0}, 2.0);
    REQUIRE(repeated);
    REQUIRE(repeated.value().normalized_value == high.value().normalized_value);
    REQUIRE(repeated.value().rgba == high.value().rgba);

    auto forged = high.value();
    forged.normalized_value = 0.25;
    REQUIRE(!forged.validate());
    forged = high.value();
    forged.rgba[0] = 0.0;
    REQUIRE(!forged.validate());
    forged = high.value();
    forged.position.z = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!forged.validate());
    forged = high.value();
    forged.probe = ProbeId{};
    REQUIRE(!forged.validate());

    const FieldVisualization auto_range{
        VisualizationId{9U}, FieldId{1U}, std::nullopt, "scalar", "gray", std::nullopt,
        std::nullopt, "", false, provenance};
    const auto zero_width = auto_range.preview_probe(ProbeId{5U}, {0.0, 0.0, 0.0}, 3.5);
    REQUIRE(zero_width);
    REQUIRE(zero_width.value().lower_bound == 3.5);
    REQUIRE(zero_width.value().upper_bound == 3.5);
    REQUIRE(zero_width.value().normalized_value == 0.5);
    REQUIRE(!zero_width.value().clipped_low);
    REQUIRE(!zero_width.value().clipped_high);
    REQUIRE(zero_width.value().rgba[0] == 0.5);

    const FieldVisualization vector_source{
        VisualizationId{10U}, FieldId{1U}, std::nullopt, "vector", "gray", 0.0, 1.0,
        "", false, provenance};
    REQUIRE(!vector_source.preview_probe(ProbeId{4U}, {0.0, 0.0, 0.0}, 0.5));
    REQUIRE(!source.preview_probe(ProbeId{}, {0.0, 0.0, 0.0}, 0.5));
    REQUIRE(!source.preview_probe(
        ProbeId{4U}, {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 0.5));
    REQUIRE(!source.preview_probe(ProbeId{4U}, {0.0, 0.0, 0.0},
                                  std::numeric_limits<double>::infinity()));
}

void field_streamline_previews_are_deterministic_and_fail_closed() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const FieldVisualization source{
        VisualizationId{11U}, FieldId{1U}, std::nullopt, "vector", "viridis", 0.0, 1.0,
        "stage:1", true, provenance};
    std::vector<FieldVectorSample> grid;
    for (std::size_t y = 0U; y < 3U; ++y) {
        for (std::size_t x = 0U; x < 3U; ++x) {
            grid.push_back(FieldVectorSample{
                {static_cast<double>(x) / 2.0, static_cast<double>(y) / 2.0, 0.0},
                {1.0, 0.0, 0.0}});
        }
    }
    const std::vector<carto::core::Vec3d> seeds{{0.1, 0.5, 0.0}, {0.5, 0.25, 0.0}};
    const auto preview = source.preview_streamlines(grid, 3U, 3U, seeds, 0.25, 4U);
    REQUIRE(preview);
    REQUIRE(preview.value().validate());
    REQUIRE(preview.value().lines.size() == seeds.size());
    REQUIRE(preview.value().lines.at(0U).points.size() == 4U);
    REQUIRE(preview.value().lines.at(0U).terminated_by_boundary);
    REQUIRE(preview.value().lines.at(0U).points.at(0U).position.x == 0.1);
    REQUIRE(preview.value().lines.at(0U).points.at(3U).position.x == 0.85);
    REQUIRE(preview.value().lines.at(0U).points.at(0U).magnitude == 1.0);
    REQUIRE(preview.value().lines.at(0U).points.at(0U).normalized_magnitude == 1.0);
    REQUIRE(preview.value().clipped_low == 0U);
    REQUIRE(preview.value().clipped_high == 0U);

    const auto repeated = source.preview_streamlines(grid, 3U, 3U, seeds, 0.25, 4U);
    REQUIRE(repeated);
    REQUIRE(repeated.value().lines.at(0U).points.size() ==
            preview.value().lines.at(0U).points.size());
    REQUIRE(repeated.value().lines.at(0U).points.at(2U).position.x ==
            preview.value().lines.at(0U).points.at(2U).position.x);

    auto gradient_grid = grid;
    for (auto& sample : gradient_grid) sample.vector = {sample.position.x, 0.0, 0.0};
    const auto interior = source.preview_streamlines(
        gradient_grid, 3U, 3U, std::vector<carto::core::Vec3d>{{0.5, 0.5, 0.0}}, 0.1, 1U);
    REQUIRE(interior);
    REQUIRE(interior.value().lines.at(0U).points.at(0U).magnitude == 0.5);

    auto forged = preview.value();
    forged.lines.at(0U).points.at(1U).magnitude = 9.0;
    REQUIRE(!forged.validate());
    forged = preview.value();
    forged.lines.at(0U).points.at(0U).position.z = 1.0;
    REQUIRE(!forged.validate());
    auto bad_grid = grid;
    bad_grid.at(1U).position.x = 0.25;
    REQUIRE(!source.preview_streamlines(bad_grid, 3U, 3U, seeds, 0.25, 4U));
    REQUIRE(!source.preview_streamlines(grid, 3U, 3U,
                                       std::vector<carto::core::Vec3d>{{1.1, 0.5, 0.0}}, 0.25, 4U));
    REQUIRE(!source.preview_streamlines(grid, 3U, 3U, seeds, 0.0, 4U));
    REQUIRE(!source.preview_streamlines(grid, 3U, 3U, seeds, 0.25, 0U));

    const FieldVisualization scalar_source{
        VisualizationId{12U}, FieldId{1U}, std::nullopt, "scalar", "gray", 0.0, 1.0,
        "", false, provenance};
    REQUIRE(!scalar_source.preview_streamlines(grid, 3U, 3U, seeds, 0.25, 4U));
    auto non_finite_grid = grid;
    non_finite_grid.at(4U).vector.x = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!source.preview_streamlines(non_finite_grid, 3U, 3U, seeds, 0.25, 4U));
    auto non_planar_grid = grid;
    non_planar_grid.at(4U).vector.z = 1.0;
    REQUIRE(!source.preview_streamlines(non_planar_grid, 3U, 3U, seeds, 0.25, 4U));

    auto stationary_grid = grid;
    for (auto& sample : stationary_grid) sample.vector = {0.0, 0.0, 0.0};
    const auto stationary = source.preview_streamlines(
        stationary_grid, 3U, 3U, std::vector<carto::core::Vec3d>{{0.5, 0.5, 0.0}}, 0.25, 4U);
    REQUIRE(stationary);
    REQUIRE(stationary.value().lines.at(0U).points.size() == 1U);
    REQUIRE(!stationary.value().lines.at(0U).terminated_by_boundary);
}

void scientific_import_receipt_round_trip_and_rejection() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();
    ScientificImportLedger ledger;
    REQUIRE(ledger.insert_receipt(ScientificImportReceipt{
        ImportId{1U}, "csv", "source://fixture/measurements.csv", "fixture-provider",
        std::nullopt, 6U, 3U, {"ignored header note"}, {"units normalized"}, provenance,
        {{"record:1", "preserved", "field:temperature", "direct scalar mapping"},
         {"record:2", "converted", "field:pressure", "units normalized"},
         {"record:3", "ignored", "", "provider metadata only"},
         {"record:4", "unsupported", "", "provider-specific encoding"},
         {"record:5", "ambiguous", "", "coordinate system missing"},
         {"record:6", "repaired", "field:velocity", "recovered bounded default"}}}));
    REQUIRE(model.set_import_ledger(std::move(ledger)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("IMPORT_LEDGER 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().import_ledger().has_value());
    REQUIRE(restored.value().import_ledger()->receipts().at(ImportId{1U}).records_admitted == 3U);
    REQUIRE(restored.value().import_ledger()->receipts().at(ImportId{1U}).preservation_report.size() == 6U);
    REQUIRE(restored.value().import_ledger()->receipts().at(ImportId{1U}).preservation_report.at(1U).disposition ==
            "converted");
    REQUIRE(restored.value().serialize() == encoded);

    ScientificImportLedger invalid_count;
    REQUIRE(!invalid_count.insert_receipt(ScientificImportReceipt{
        ImportId{1U}, "csv", "source://fixture", "fixture-provider", std::nullopt,
        1U, 2U, {}, {}, provenance, {}}));
    ScientificImportLedger duplicate_notes;
    REQUIRE(!duplicate_notes.insert_receipt(ScientificImportReceipt{
        ImportId{1U}, "csv", "source://fixture", "fixture-provider", std::nullopt,
        1U, 1U, {"same", "same"}, {}, provenance, {}}));
    ScientificImportLedger unknown_disposition;
    REQUIRE(!unknown_disposition.insert_receipt(ScientificImportReceipt{
        ImportId{1U}, "csv", "source://fixture", "fixture-provider", std::nullopt,
        1U, 1U, {}, {}, provenance,
        {{"record:1", "reinterpreted", "phenotype:truth", "must be rejected"}}}));
    ScientificImportLedger duplicate_source_keys;
    REQUIRE(!duplicate_source_keys.insert_receipt(ScientificImportReceipt{
        ImportId{1U}, "csv", "source://fixture", "fixture-provider", std::nullopt,
        2U, 2U, {}, {}, provenance,
        {{"record:1", "preserved", "field:a", ""},
         {"record:1", "converted", "field:b", "duplicate source key"}}}));
    ScientificImportLedger legacy_ledger;
    REQUIRE(legacy_ledger.insert_receipt(ScientificImportReceipt{
        ImportId{1U}, "csv", "source://fixture/legacy", "fixture-provider", std::nullopt,
        1U, 1U, {}, {}, provenance, {}}));
    std::string legacy_encoded = legacy_ledger.serialize();
    const auto legacy_header = legacy_encoded.find("CARTOGRAPHER_SCIENTIFIC_IMPORTS 2");
    REQUIRE(legacy_header != std::string::npos);
    legacy_encoded.replace(legacy_header, std::string("CARTOGRAPHER_SCIENTIFIC_IMPORTS 2").size(),
                           "CARTOGRAPHER_SCIENTIFIC_IMPORTS 1");
    const auto import_line = legacy_encoded.find("IMPORT ");
    const auto import_line_end = legacy_encoded.find('\n', import_line);
    REQUIRE(import_line != std::string::npos && import_line_end != std::string::npos);
    const auto legacy_report_count = legacy_encoded.rfind(" 0", import_line_end);
    REQUIRE(legacy_report_count != std::string::npos && legacy_report_count > import_line);
    legacy_encoded.erase(legacy_report_count, 2U);
    const auto legacy_restored = ScientificImportLedger::deserialize(legacy_encoded);
    REQUIRE(legacy_restored);
    REQUIRE(legacy_restored.value().receipts().at(ImportId{1U}).preservation_report.empty());
    REQUIRE(!ScientificImportLedger::deserialize(
        "CARTOGRAPHER_SCIENTIFIC_IMPORTS 999\nEND\n"));
}

void phenotype_capability_trace_round_trip_and_rejection() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();

    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://fixture-development";
    program.description = "capability trace fixture";
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "formation", "always", provenance}));
    REQUIRE(program.insert_instruction(DevelopmentInstruction{
        InstructionId{1U}, StageId{1U}, "form-sensor", "morphogenesis", "always",
        {ParameterId{1U}}, {StructureId{2U}}, {}, provenance}));
    REQUIRE(model.set_developmental_program(std::move(program)));

    DevelopmentalTrace trace;
    trace.program_reference = "program://fixture-development";
    trace.genome_reference = "program://fixture-development";
    REQUIRE(trace.insert_entry(DevelopmentTraceEntry{
        TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{2U}}, {},
        "receipt://development/sensor", provenance, std::nullopt, {},
        std::nullopt, std::nullopt}));
    REQUIRE(model.set_developmental_trace(std::move(trace)));

    PhenotypeCapabilityModel phenotype;
    phenotype.schema = "cartographer.phenotype-capabilities.v1";
    phenotype.compilation_reference = "provider://menagerie/phenotype-preview";
    phenotype.status = "provider_receipt";
    phenotype.provenance = provenance;
    REQUIRE(phenotype.insert_capability(PhenotypeCapability{
        CapabilityId{1U}, "chemical sensing", "sensory", "present", 0.75, "normalized",
        {StructureId{2U}}, {GeneId{2U}}, {InstructionId{1U}}, {TraceId{1U}},
        "gene 2 -> instruction 1 -> structure 2 -> receptor capability", provenance}));
    REQUIRE(model.set_phenotype_capabilities(std::move(phenotype)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("PHENOTYPE_CAPABILITIES 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().phenotype_capabilities().has_value());
    const auto& capability = restored.value().phenotype_capabilities()->capabilities().at(
        CapabilityId{1U});
    REQUIRE(capability.contributing_genes == std::vector<GeneId>{GeneId{2U}});
    REQUIRE(capability.contributing_instructions == std::vector<InstructionId>{InstructionId{1U}});
    REQUIRE(capability.contributing_traces == std::vector<TraceId>{TraceId{1U}});
    const auto forward_trace = restored.value().phenotype_capabilities()->trace(CapabilityId{1U});
    REQUIRE(forward_trace);
    REQUIRE(forward_trace.value().name == "chemical sensing");
    REQUIRE(forward_trace.value().contributing_structures == std::vector<StructureId>{StructureId{2U}});
    const auto structure_trace = restored.value().phenotype_capabilities()->capabilities_for_structure(
        StructureId{2U});
    REQUIRE(structure_trace);
    REQUIRE(structure_trace.value() == std::vector<CapabilityId>{CapabilityId{1U}});
    const auto gene_trace = restored.value().phenotype_capabilities()->capabilities_for_gene(GeneId{2U});
    REQUIRE(gene_trace);
    REQUIRE(gene_trace.value() == std::vector<CapabilityId>{CapabilityId{1U}});
    const auto instruction_trace = restored.value().phenotype_capabilities()->capabilities_for_instruction(
        InstructionId{1U});
    REQUIRE(instruction_trace);
    REQUIRE(instruction_trace.value() == std::vector<CapabilityId>{CapabilityId{1U}});
    const auto development_trace = restored.value().phenotype_capabilities()->capabilities_for_trace(
        TraceId{1U});
    REQUIRE(development_trace);
    REQUIRE(development_trace.value() == std::vector<CapabilityId>{CapabilityId{1U}});
    REQUIRE(structure_trace.value() ==
            restored.value().phenotype_capabilities()->capabilities_for_structure(StructureId{2U}).value());
    REQUIRE(restored.value().phenotype_capabilities()->capabilities_for_structure(StructureId{999U}));
    REQUIRE(restored.value().phenotype_capabilities()->capabilities_for_structure(StructureId{999U}).value().empty());
    REQUIRE(!restored.value().phenotype_capabilities()->trace(CapabilityId{999U}));
    REQUIRE(!restored.value().phenotype_capabilities()->trace(CapabilityId{}));
    REQUIRE(!restored.value().phenotype_capabilities()->capabilities_for_gene(GeneId{}));
    REQUIRE(restored.value().serialize() == encoded);

    PhenotypeCapabilityModel no_source;
    no_source.schema = "cartographer.phenotype-capabilities.v1";
    no_source.compilation_reference = "provider://fixture";
    no_source.status = "preview";
    no_source.provenance = provenance;
    REQUIRE(!no_source.insert_capability(PhenotypeCapability{
        CapabilityId{1U}, "orphan", "behavioral", "present", std::nullopt, "", {}, {}, {}, {},
        "missing source trace", provenance}));

    PhenotypeCapabilityModel missing_provenance;
    missing_provenance.schema = "cartographer.phenotype-capabilities.v1";
    missing_provenance.compilation_reference = "provider://fixture";
    missing_provenance.status = "preview";
    missing_provenance.provenance = provenance;
    REQUIRE(!missing_provenance.insert_capability(PhenotypeCapability{
        CapabilityId{1U}, "unprovenanced", "behavioral", "present", std::nullopt, "",
        {StructureId{1U}}, {}, {}, {}, "structure-only", Provenance{}}));

    PhenotypeCapabilityModel stale;
    stale.schema = "cartographer.phenotype-capabilities.v1";
    stale.compilation_reference = "provider://fixture";
    stale.status = "preview";
    stale.provenance = provenance;
    REQUIRE(stale.insert_capability(PhenotypeCapability{
        CapabilityId{1U}, "stale", "locomotion", "present", 1.0, "ratio", {},
        {GeneId{999U}}, {}, {}, "unknown gene", provenance}));
    auto stale_model = make_model();
    REQUIRE(stale_model.set_phenotype_capabilities(std::move(stale)));
    REQUIRE(!stale_model.validate());

    REQUIRE(!PhenotypeCapabilityModel::deserialize(
        "CARTOGRAPHER_PHENOTYPE_CAPABILITIES 999\nEND\n"));
    REQUIRE(!PhenotypeCapabilityModel::deserialize(
        "CARTOGRAPHER_PHENOTYPE_CAPABILITIES 1\nEND\n"));
}

void gene_regulatory_network_round_trip_and_rejection() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();

    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://fixture-development";
    program.description = "regulatory network fixture";
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "expression", "always", provenance}));
    REQUIRE(model.set_developmental_program(std::move(program)));

    GeneRegulatoryNetwork network;
    network.schema = "cartographer.gene-regulatory.v1";
    network.reference = "provider://fixture-regulatory-network";
    network.status = "provider_receipt";
    network.provenance = provenance;
    REQUIRE(network.insert_element(RegulatoryElement{
        RegulatoryElementId{1U}, GeneId{1U}, "sensor enhancer", "enhancer",
        "source://fixture/enhancer-1", provenance}));
    REQUIRE(network.insert_edge(RegulatoryEdge{
        RegulatoryEdgeId{1U}, GeneId{1U}, GeneId{2U}, RegulatoryElementId{1U},
        "activation", "growth-rate > 0", StageId{1U}, provenance}));
    REQUIRE(network.insert_expression(SpatialExpression{
        ExpressionId{1U}, GeneId{2U}, std::nullopt, std::nullopt, FieldId{1U}, StageId{1U},
        "continuous_field", 0.5, "", "normalized", provenance}));
    REQUIRE(model.set_gene_regulatory_network(std::move(network)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("GENE_REGULATORY_NETWORK 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().gene_regulatory_network().has_value());
    REQUIRE(restored.value().gene_regulatory_network()->edges().at(RegulatoryEdgeId{1U}).mode ==
            "activation");
    REQUIRE(restored.value().gene_regulatory_network()->expressions().at(ExpressionId{1U}).field ==
            FieldId{1U});
    REQUIRE(restored.value().serialize() == encoded);

    GeneRegulatoryNetwork invalid_mode;
    invalid_mode.schema = "cartographer.gene-regulatory.v1";
    invalid_mode.reference = "provider://fixture";
    invalid_mode.status = "preview";
    invalid_mode.provenance = provenance;
    REQUIRE(!invalid_mode.insert_edge(RegulatoryEdge{
        RegulatoryEdgeId{1U}, GeneId{1U}, GeneId{2U}, RegulatoryElementId{1U}, "unknown", "", {},
        provenance}));

    GeneRegulatoryNetwork two_targets;
    two_targets.schema = "cartographer.gene-regulatory.v1";
    two_targets.reference = "provider://fixture";
    two_targets.status = "preview";
    two_targets.provenance = provenance;
    REQUIRE(!two_targets.insert_expression(SpatialExpression{
        ExpressionId{1U}, GeneId{1U}, StructureId{1U}, RegionId{1U}, std::nullopt, std::nullopt,
        "structure_scalar", 1.0, "", "ratio", provenance}));

    GeneRegulatoryNetwork orphan_edge;
    orphan_edge.schema = "cartographer.gene-regulatory.v1";
    orphan_edge.reference = "provider://fixture";
    orphan_edge.status = "preview";
    orphan_edge.provenance = provenance;
    REQUIRE(orphan_edge.insert_edge(RegulatoryEdge{
        RegulatoryEdgeId{1U}, GeneId{1U}, GeneId{2U}, RegulatoryElementId{99U},
        "activation", "", std::nullopt, provenance}));
    REQUIRE(!orphan_edge.validate());

    GeneRegulatoryNetwork stale_gene;
    stale_gene.schema = "cartographer.gene-regulatory.v1";
    stale_gene.reference = "provider://fixture";
    stale_gene.status = "preview";
    stale_gene.provenance = provenance;
    REQUIRE(stale_gene.insert_element(RegulatoryElement{
        RegulatoryElementId{1U}, GeneId{999U}, "stale", "enhancer", "source://stale", provenance}));
    auto stale_model = make_model();
    REQUIRE(stale_model.set_gene_regulatory_network(std::move(stale_gene)));
    REQUIRE(!stale_model.validate());

    REQUIRE(!GeneRegulatoryNetwork::deserialize(
        "CARTOGRAPHER_GENE_REGULATORY_NETWORK 999\nEND\n"));
}

void anatomy_provider_manifest_round_trip_and_rejection() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();

    AnatomyModel anatomy;
    anatomy.schema = "cartographer.anatomy.v1";
    REQUIRE(anatomy.insert_term(AnatomyTerm{
        AnatomyTermId{1U}, "UBERON", "UBERON:0000948", "heart", "organ", std::nullopt,
        "fixture organ term", provenance, "uberon-2026-01", "human-atlas-release-1"}));
    REQUIRE(anatomy.insert_part(AnatomyPart{
        AnatomyPartId{1U}, StructureId{2U}, AnatomyTermId{1U}, std::nullopt, "organ-part",
        provenance}));
    REQUIRE(model.set_anatomy(std::move(anatomy)));

    AnatomyProviderManifest manifest;
    manifest.schema = "cartographer.anatomy-provider-manifest.v1";
    manifest.provider = "vanta.anatomy-provider";
    manifest.publication_reference = "candidate://vanta/fixture-heart";
    manifest.status = "candidate";
    manifest.source_model_revision = carto::core::Revision{4U};
    const auto source_manifest_digest = carto::assets::Sha256Digest::from_hex(
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    const auto geometry_digest = carto::assets::Sha256Digest::from_hex(
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    REQUIRE(source_manifest_digest);
    REQUIRE(geometry_digest);
    manifest.source_manifest_digest = source_manifest_digest.value();
    manifest.provenance = provenance;
    REQUIRE(manifest.insert_binding(AnatomyProviderBinding{
        AnatomyBindingId{1U}, AnatomyPartId{1U}, "UBERON:0000948", "left",
        GeometryBinding{"mesh", "provider://fixture/heart"}, "2026.10", "CC-BY-4.0",
        geometry_digest.value(), "cardiac-muscle", "edit://cartographer/heart-1", provenance}));
    REQUIRE(manifest.validate_handoff_candidate(carto::core::Revision{4U}));
    REQUIRE(!manifest.validate_handoff_candidate(carto::core::Revision{5U}));
    REQUIRE(!manifest.validate_handoff_candidate(carto::core::Revision{}));
    const auto candidate = AnatomyProviderCandidate::from_manifest(
        manifest, carto::core::Revision{4U});
    REQUIRE(candidate);
    const auto candidate_encoded = candidate.value().serialize();
    REQUIRE(!candidate_encoded.empty());
    const auto candidate_restored = AnatomyProviderCandidate::deserialize(candidate_encoded);
    REQUIRE(candidate_restored);
    REQUIRE(candidate_restored.value().manifest.serialize() == manifest.serialize());
    REQUIRE(candidate_restored.value().manifest_digest == candidate.value().manifest_digest);
    auto forged_candidate = candidate.value();
    forged_candidate.manifest.status = "tampered";
    REQUIRE(forged_candidate.serialize().empty());
    auto tampered_candidate = candidate_encoded;
    const auto digest_offset = tampered_candidate.find(candidate.value().manifest_digest.hex());
    REQUIRE(digest_offset != std::string::npos);
    tampered_candidate[digest_offset] =
        tampered_candidate[digest_offset] == '0' ? '1' : '0';
    REQUIRE(!AnatomyProviderCandidate::deserialize(tampered_candidate));
    auto future_candidate = candidate_encoded;
    const auto candidate_header = future_candidate.find(
        "CARTOGRAPHER_ANATOMY_PROVIDER_CANDIDATE 1");
    REQUIRE(candidate_header != std::string::npos);
    future_candidate.replace(candidate_header,
                             std::string("CARTOGRAPHER_ANATOMY_PROVIDER_CANDIDATE 1").size(),
                             "CARTOGRAPHER_ANATOMY_PROVIDER_CANDIDATE 999");
    REQUIRE(!AnatomyProviderCandidate::deserialize(future_candidate));
    REQUIRE(!AnatomyProviderCandidate::from_manifest(
        manifest, carto::core::Revision{5U}));
    auto missing_manifest_digest = manifest;
    missing_manifest_digest.source_manifest_digest.reset();
    REQUIRE(!missing_manifest_digest.validate_handoff_candidate(carto::core::Revision{4U}));
    AnatomyProviderManifest missing_geometry_digest;
    missing_geometry_digest.schema = "cartographer.anatomy-provider-manifest.v1";
    missing_geometry_digest.provider = "vanta.anatomy-provider";
    missing_geometry_digest.publication_reference = "candidate://vanta/fixture-heart";
    missing_geometry_digest.status = "candidate";
    missing_geometry_digest.source_model_revision = carto::core::Revision{4U};
    missing_geometry_digest.source_manifest_digest = source_manifest_digest.value();
    missing_geometry_digest.provenance = provenance;
    REQUIRE(missing_geometry_digest.insert_binding(AnatomyProviderBinding{
        AnatomyBindingId{1U}, AnatomyPartId{1U}, "UBERON:0000948", "left",
        GeometryBinding{"mesh", "provider://fixture/heart"}, "2026.10", "CC-BY-4.0",
        std::nullopt, "cardiac-muscle", "edit://cartographer/heart-1", provenance}));
    REQUIRE(!missing_geometry_digest.validate_handoff_candidate(carto::core::Revision{4U}));
    REQUIRE(model.set_anatomy_provider_manifest(std::move(manifest)));
    REQUIRE(model.validate());
    REQUIRE(model.validate_anatomy_provider_candidate(carto::core::Revision{4U}));
    REQUIRE(!model.validate_anatomy_provider_candidate(carto::core::Revision{5U}));
    const auto model_candidate = model.build_anatomy_provider_candidate(
        carto::core::Revision{4U});
    REQUIRE(model_candidate);
    REQUIRE(model_candidate.value().serialize() == candidate_encoded);
    REQUIRE(!model.build_anatomy_provider_candidate(carto::core::Revision{5U}));

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("ANATOMY_PROVIDER_MANIFEST 2") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().anatomy_provider_manifest().has_value());
    REQUIRE(restored.value().anatomy_provider_manifest()->source_model_revision.has_value());
    REQUIRE(restored.value().anatomy_provider_manifest()->source_model_revision->value() == 4U);
    REQUIRE(restored.value().anatomy_provider_manifest()->bindings().at(
                AnatomyBindingId{1U}).semantic_id == "UBERON:0000948");
    REQUIRE(restored.value().serialize() == encoded);

    const auto legacy_manifest = AnatomyProviderManifest::deserialize(
        "CARTOGRAPHER_ANATOMY_PROVIDER_MANIFEST 1\n"
        "SCHEMA \"cartographer.anatomy-provider-manifest.v1\"\n"
        "PROVIDER \"fixture-provider\"\n"
        "PUBLICATION_REFERENCE \"candidate://fixture\"\n"
        "STATUS \"candidate\"\n"
        "SOURCE_MANIFEST_DIGEST 0\n"
        "PROVENANCE \"fixture://scientific-model\" \"2026.10\" \"CC0-1.0\" "
        "\"cartographer-test-provider\" \"2026-10-03T00:00:00Z\" 0\n"
        "BINDINGS 0\n"
        "END\n");
    REQUIRE(legacy_manifest);
    REQUIRE(!legacy_manifest.value().source_model_revision.has_value());

    AnatomyProviderManifest missing_geometry;
    missing_geometry.schema = "cartographer.anatomy-provider-manifest.v1";
    missing_geometry.provider = "fixture-provider";
    missing_geometry.publication_reference = "candidate://fixture";
    missing_geometry.status = "candidate";
    missing_geometry.provenance = provenance;
    REQUIRE(!missing_geometry.insert_binding(AnatomyProviderBinding{
        AnatomyBindingId{1U}, AnatomyPartId{1U}, "TERM:1", "unspecified", GeometryBinding{},
        "2026.10", "CC0-1.0", std::nullopt, "", "", provenance}));

    AnatomyProviderManifest zero_revision;
    zero_revision.schema = "cartographer.anatomy-provider-manifest.v1";
    zero_revision.provider = "fixture-provider";
    zero_revision.publication_reference = "candidate://fixture";
    zero_revision.status = "candidate";
    zero_revision.source_model_revision = carto::core::Revision{};
    zero_revision.provenance = provenance;
    REQUIRE(!zero_revision.validate());

    AnatomyProviderManifest stale_part;
    stale_part.schema = "cartographer.anatomy-provider-manifest.v1";
    stale_part.provider = "fixture-provider";
    stale_part.publication_reference = "candidate://fixture";
    stale_part.status = "candidate";
    stale_part.provenance = provenance;
    REQUIRE(stale_part.insert_binding(AnatomyProviderBinding{
        AnatomyBindingId{1U}, AnatomyPartId{999U}, "TERM:999", "unspecified",
        GeometryBinding{"mesh", "provider://fixture/unknown"}, "2026.10", "CC0-1.0",
        std::nullopt, "", "", provenance}));
    auto stale_model = make_model();
    REQUIRE(stale_model.set_anatomy_provider_manifest(std::move(stale_part)));
    REQUIRE(!stale_model.validate());

    REQUIRE(!AnatomyProviderManifest::deserialize(
        "CARTOGRAPHER_ANATOMY_PROVIDER_MANIFEST 999\nEND\n"));
}

void scientific_replay_capsule_round_trip_and_rejection() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    const auto external_digest = carto::assets::Sha256Digest::from_hex(
        "1111111111111111111111111111111111111111111111111111111111111111");
    const auto expected_digest = carto::assets::Sha256Digest::from_hex(
        "2222222222222222222222222222222222222222222222222222222222222222");
    REQUIRE(external_digest);
    REQUIRE(expected_digest);

    auto model = make_model();
    ScientificReplayCapsuleModel replay;
    replay.schema = "cartographer.scientific-replay.v1";
    replay.reference = "replay://fixture/organism";
    replay.status = "authoring_capsule";
    replay.provenance = provenance;
    REQUIRE(replay.insert_capsule(ScientificReplayCapsule{
        ReplayId{1U}, carto::core::Revision{4U}, "reference-provider", "solver-1.0",
        {42U, 7U}, {ParameterId{1U}}, {external_digest.value()}, {expected_digest.value()},
        provenance}));
    REQUIRE(model.set_replay_capsules(std::move(replay)));
    REQUIRE(model.validate());

    const auto encoded = model.serialize();
    REQUIRE(encoded.find("REPLAY_CAPSULES 1") != std::string::npos);
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().replay_capsules().has_value());
    const auto& capsule = restored.value().replay_capsules()->capsules().at(ReplayId{1U});
    REQUIRE(capsule.source_model_revision == carto::core::Revision{4U});
    REQUIRE(capsule.parameters == std::vector<ParameterId>{ParameterId{1U}});
    REQUIRE(capsule.expected_output_digests.size() == 1U);
    REQUIRE(restored.value().serialize() == encoded);

    ScientificReplayCapsuleModel no_revision;
    no_revision.schema = "cartographer.scientific-replay.v1";
    no_revision.reference = "replay://fixture";
    no_revision.status = "authoring_capsule";
    no_revision.provenance = provenance;
    REQUIRE(!no_revision.insert_capsule(ScientificReplayCapsule{
        ReplayId{1U}, carto::core::Revision{}, "provider", "solver", {}, {},
        {external_digest.value()}, {expected_digest.value()}, provenance}));

    ScientificReplayCapsuleModel duplicate_digest;
    duplicate_digest.schema = "cartographer.scientific-replay.v1";
    duplicate_digest.reference = "replay://fixture";
    duplicate_digest.status = "authoring_capsule";
    duplicate_digest.provenance = provenance;
    REQUIRE(!duplicate_digest.insert_capsule(ScientificReplayCapsule{
        ReplayId{1U}, carto::core::Revision{1U}, "provider", "solver", {}, {},
        {external_digest.value(), external_digest.value()}, {}, provenance}));

    ScientificReplayCapsuleModel stale_parameter;
    stale_parameter.schema = "cartographer.scientific-replay.v1";
    stale_parameter.reference = "replay://fixture";
    stale_parameter.status = "authoring_capsule";
    stale_parameter.provenance = provenance;
    REQUIRE(stale_parameter.insert_capsule(ScientificReplayCapsule{
        ReplayId{1U}, carto::core::Revision{1U}, "provider", "solver", {}, {ParameterId{999U}},
        {}, {}, provenance}));
    auto stale_model = make_model();
    REQUIRE(stale_model.set_replay_capsules(std::move(stale_parameter)));
    REQUIRE(!stale_model.validate());

    REQUIRE(!ScientificReplayCapsuleModel::deserialize(
        "CARTOGRAPHER_SCIENTIFIC_REPLAY_CAPSULES 999\nEND\n"));
}

void scientific_model_rejects_cycles_dangling_references_and_unsafe_values() {
    using namespace carto::scientific;
    ScientificModel cycle;
    REQUIRE(cycle.insert_region(Region{
        RegionId{1U}, "a", GeometryBinding{}, RegionId{2U}, "", {}, {}}));
    REQUIRE(cycle.insert_region(Region{
        RegionId{2U}, "b", GeometryBinding{}, RegionId{1U}, "", {}, {}}));
    REQUIRE(!cycle.validate());

    ScientificModel dangling;
    REQUIRE(dangling.insert_region(Region{
        RegionId{1U}, "a", GeometryBinding{}, std::nullopt, "", {}, {}}));
    REQUIRE(dangling.insert_field(Field{
        FieldId{1U}, FieldKind::scalar, Association::continuous, "", "", "",
        std::nullopt, RegionId{999U}, "", std::nullopt, Provenance{}}));
    REQUIRE(!dangling.validate());

    ScientificModel duplicate;
    REQUIRE(duplicate.insert_region(Region{
        RegionId{1U}, "a", GeometryBinding{}, std::nullopt, "", {}, {}}));
    REQUIRE(!duplicate.insert_region(Region{
        RegionId{1U}, "b", GeometryBinding{}, std::nullopt, "", {}, {}}));
    REQUIRE(!duplicate.insert_parameter(Parameter{
        ParameterId{2U}, "bad", "", 0.0,
        -std::numeric_limits<double>::infinity(), std::nullopt, Provenance{}}));
    REQUIRE(!duplicate.insert_region(Region{
        RegionId{3U}, "bad-date", GeometryBinding{}, std::nullopt, "", {},
        Provenance{"fixture://bad", "1", "CC0", "provider", "2026-10-03T00:00:00", std::nullopt}}));

    const auto valid = make_model().serialize();
    REQUIRE(!ScientificModel::deserialize(valid + "trailing"));
    REQUIRE(!ScientificModel::deserialize(
        "CARTOGRAPHER_SCIENTIFIC_MODEL 999\nEND\n"));
}

void genome_mutation_lineage_acceptance_benchmark() {
    using namespace carto::scientific;
    const auto provenance = fixture_provenance();
    auto model = make_model();
    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://fixture-development";
    program.description = "lineage benchmark program";
    REQUIRE(program.insert_stage(DevelopmentStage{
        StageId{1U}, 0U, "mutation-stage", "always", provenance}));
    REQUIRE(program.insert_instruction(DevelopmentInstruction{
        InstructionId{2U}, StageId{1U}, "adjust", "mutation", "always",
        {ParameterId{1U}}, {}, {}, provenance}));
    REQUIRE(model.set_developmental_program(std::move(program)));
    LineageGraph lineage;
    for (std::uint64_t generation = 0U; generation <= 100U; ++generation) {
        const LineageId lineage_id{generation + 1U};
        const std::optional<LineageId> parent = generation == 0U
            ? std::nullopt
            : std::optional<LineageId>{LineageId{generation}};
        REQUIRE(lineage.insert_node(LineageNode{
            lineage_id, parent, std::nullopt, generation, std::nullopt, std::nullopt,
            provenance, {}}));
        if (generation > 0U) {
            REQUIRE(lineage.insert_mutation(MutationReceipt{
                MutationId{generation}, LineageId{generation}, lineage_id,
                std::nullopt, std::nullopt, 42000U + generation,
                "deterministic-lineage-benchmark", {GeneId{1U}}, {InstructionId{2U}}, {},
                "authored receipt; not executed", provenance}));
        }
    }
    REQUIRE(model.set_lineage_graph(std::move(lineage)));
    REQUIRE(model.validate());
    REQUIRE(model.lineage_graph()->nodes().size() == 101U);
    REQUIRE(model.lineage_graph()->mutations().size() == 100U);
    REQUIRE(model.lineage_graph()->nodes().at(LineageId{101U}).generation == 100U);
    REQUIRE(model.lineage_graph()->mutations().at(MutationId{100U}).seed == 42100U);

    const auto encoded = model.serialize();
    const auto restored = ScientificModel::deserialize(encoded);
    REQUIRE(restored);
    REQUIRE(restored.value().lineage_graph()->nodes().size() == 101U);
    REQUIRE(restored.value().lineage_graph()->mutations().size() == 100U);
    REQUIRE(restored.value().serialize() == encoded);

    LineageGraph invalid_child;
    REQUIRE(invalid_child.insert_node(LineageNode{
        LineageId{1U}, std::nullopt, std::nullopt, 0U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(invalid_child.insert_node(LineageNode{
        LineageId{2U}, LineageId{1U}, std::nullopt, 1U, std::nullopt, std::nullopt,
        provenance, {}}));
    REQUIRE(invalid_child.insert_mutation(MutationReceipt{
        MutationId{1U}, LineageId{2U}, LineageId{1U}, std::nullopt, std::nullopt, 1U,
        "forged-parent", {GeneId{1U}}, {InstructionId{2U}}, {},
        "must reject", provenance}));
    REQUIRE(!invalid_child.validate());
}

void scientific_model_mutation_is_journaled_and_reopens() {
    TempDirectory temp;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(temp.path() / "journal.log");
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "scientific-author");
    REQUIRE(transaction);
    REQUIRE(transaction.value().set_scientific_model(make_model()));
    const auto committed = transaction.value().commit("scientific.model.bind");
    REQUIRE(committed);
    REQUIRE(document.revision() == carto::core::Revision{1U});
    REQUIRE(document.scientific_model().studies().size() == 1U);

    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    const std::string payload(entries.value().front().payload.begin(), entries.value().front().payload.end());
    const std::string current_version = "CARTOGRAPHER_PROJECT " +
        std::to_string(carto::project::ProjectDocument::kSchemaVersion);
    REQUIRE(payload.find(current_version) != std::string::npos);
    REQUIRE(payload.find("SCIENTIFIC_MODEL") != std::string::npos);

    const auto path = temp.path() / "scientific.carto";
    REQUIRE(document.save_atomic(path));
    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().scientific_model().regions().size() == 2U);
    REQUIRE(reopened.value().scientific_model().morphology().has_value());
    REQUIRE(reopened.value().scientific_model().functional_genome().has_value());
    REQUIRE(reopened.value().serialize() == document.serialize());

    std::string missing_scientific = document.serialize();
    const auto scientific_start = missing_scientific.find("SCIENTIFIC_MODEL ");
    REQUIRE(scientific_start != std::string::npos);
    const auto scientific_end = missing_scientific.rfind("\nEND\n");
    REQUIRE(scientific_end != std::string::npos);
    missing_scientific.erase(scientific_start, scientific_end - scientific_start + 1U);
    REQUIRE(!carto::project::ProjectDocument::deserialize(missing_scientific));

    std::string legacy_v4 = missing_scientific;
    const auto current_header = legacy_v4.find(current_version);
    REQUIRE(current_header != std::string::npos);
    legacy_v4.replace(current_header, current_version.size(),
                      "CARTOGRAPHER_PROJECT 4");
    const auto loaded_v4 = carto::project::ProjectDocument::deserialize(legacy_v4);
    REQUIRE(loaded_v4);
    REQUIRE(loaded_v4.value().scientific_model().empty());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests = {
        {"scientific model round trips deterministically", scientific_model_round_trips_deterministically},
        {"artificial organism authoring chain round trips with previews", artificial_organism_authoring_chain_round_trips_with_previews},
        {"morphology cross-domain archetypes round trip", morphology_cross_domain_archetypes_round_trip},
        {"evolutionary morphospace preview is deterministic and fail closed", evolutionary_morphospace_preview_is_deterministic_and_fail_closed},
        {"parameter sweep preview is deterministic, bounded, and fail closed", parameter_sweep_preview_is_deterministic_bounded_and_fail_closed},
        {"functional genome provenance codec preserves v1 and rejects future", functional_genome_provenance_codec_preserves_v1_and_rejects_future},
        {"functional genome exchange preserves semantics and rejects tampering", functional_genome_exchange_preserves_semantics_and_rejects_tampering},
        {"developmental program and lineage round trip with boundaries", developmental_program_and_lineage_round_trip_with_boundaries},
        {"developmental program diff is structural and deterministic", developmental_program_diff_is_structural_and_deterministic},
        {"developmental and lineage hostile records are rejected", developmental_and_lineage_deserialization_rejects_hostile_records},
        {"lineage Newick exchange preserves identity and fails closed", lineage_newick_exchange_preserves_identity_and_fails_closed},
        {"morphometrics round trip and reference checks", morphometrics_round_trip_and_reference_checks},
        {"bilateral morphology landmark preview is deterministic and fail closed", bilateral_morphology_landmark_preview_is_deterministic_and_fail_closed},
        {"morphology landmark exchange is reference bound and fail closed", morphology_landmark_exchange_is_reference_bound_and_fail_closed},
        {"morphology growth domain round trip and rejection", morphology_growth_domain_round_trip_and_rejection},
        {"sequence genome round trip and reference checks", sequence_genome_round_trip_and_reference_checks},
        {"sequence genome GFF3 exchange is reference bound and fail closed", sequence_genome_gff3_exchange_is_reference_bound_and_fail_closed},
        {"sequence genome BED6 exchange is reference bound and fail closed", sequence_genome_bed6_exchange_is_reference_bound_and_fail_closed},
        {"sequence genome GTF exchange is reference bound and fail closed", sequence_genome_gtf_exchange_is_reference_bound_and_fail_closed},
        {"anatomy ontology table exchange is strict and semantic only", anatomy_ontology_table_exchange_is_strict_and_semantic_only},
        {"anatomy partonomy round trip and reference checks", anatomy_partonomy_round_trip_and_reference_checks},
        {"field visualization round trip and reference checks", field_visualization_round_trip_and_reference_checks},
        {"field slice and contour previews are deterministic and fail closed", field_slice_and_contour_previews_are_deterministic_and_fail_closed},
        {"field isosurface previews are deterministic and fail closed", field_isosurface_previews_are_deterministic_and_fail_closed},
        {"field volume previews are deterministic and fail closed", field_volume_previews_are_deterministic_and_fail_closed},
        {"field probe previews are deterministic and fail closed", field_probe_previews_are_deterministic_and_fail_closed},
        {"field streamline previews are deterministic and fail closed", field_streamline_previews_are_deterministic_and_fail_closed},
        {"scientific import receipt round trip and rejection", scientific_import_receipt_round_trip_and_rejection},
        {"phenotype capability trace round trip and rejection", phenotype_capability_trace_round_trip_and_rejection},
        {"gene regulatory network round trip and rejection", gene_regulatory_network_round_trip_and_rejection},
        {"anatomy provider manifest round trip and rejection", anatomy_provider_manifest_round_trip_and_rejection},
        {"scientific replay capsule round trip and rejection", scientific_replay_capsule_round_trip_and_rejection},
        {"scientific model rejects unsafe references and values", scientific_model_rejects_cycles_dangling_references_and_unsafe_values},
        {"genome mutation lineage acceptance benchmark", genome_mutation_lineage_acceptance_benchmark},
        {"scientific model mutation is journaled and reopens", scientific_model_mutation_is_journaled_and_reopens},
    };
    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
