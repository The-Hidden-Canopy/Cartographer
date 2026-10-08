#include <carto/io/obj.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/application/application.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/providers/registry.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::core::Result;

constexpr std::uintmax_t kFunctionalGenomeExchangeMaxBytes = 8U * 1024U * 1024U;
constexpr std::uintmax_t kAnatomyProviderCandidateMaxBytes = 8U * 1024U * 1024U;
constexpr std::uintmax_t kSequenceGenomeFastaMaxBytes = 20U * 1024U * 1024U;
constexpr std::uintmax_t kSequenceGenomeVcfMaxBytes = 20U * 1024U * 1024U;
constexpr std::uintmax_t kSequenceGenomeGff3MaxBytes = 20U * 1024U * 1024U;
constexpr std::uintmax_t kSequenceGenomeBed6MaxBytes = 20U * 1024U * 1024U;
constexpr std::uintmax_t kSequenceGenomeGtfMaxBytes = 20U * 1024U * 1024U;
constexpr std::uintmax_t kLineageNewickMaxBytes = 4U * 1024U * 1024U;
constexpr std::uintmax_t kAnatomyOntologyTableMaxBytes = 4U * 1024U * 1024U;
constexpr std::uintmax_t kMorphologyLandmarkSetMaxBytes = 16U * 1024U * 1024U;

void print_error(const carto::core::Diagnostic& diagnostic) {
    std::cerr << "error[" << carto::core::error_code_name(diagnostic.code) << "]: "
              << diagnostic.message << '\n';
    for (const auto& context : diagnostic.context) {
        std::cerr << "  context: " << context << '\n';
    }
}

Result<std::string> read_bounded_text(
    const std::filesystem::path& path,
    std::uintmax_t maximum_bytes,
    std::string_view label) {
    std::error_code status_error;
    const auto bytes = std::filesystem::file_size(path, status_error);
    if (status_error) {
        return Result<std::string>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "unable to inspect " + std::string(label) + " size"));
    }
    if (bytes > maximum_bytes) {
        return Result<std::string>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::validation_failed,
            std::string(label) + " exceeds its safety limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::string>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "unable to open " + std::string(label) + " for reading"));
    }
    std::string contents(static_cast<std::size_t>(bytes), '\0');
    if (bytes != 0U) {
        input.read(contents.data(), static_cast<std::streamsize>(bytes));
    }
    if (input.gcount() != static_cast<std::streamsize>(bytes) ||
        (!input.good() && !input.eof())) {
        return Result<std::string>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "failed while reading " + std::string(label)));
    }
    return Result<std::string>::success(std::move(contents));
}

Result<void> write_new_text_atomically(
    const std::filesystem::path& path,
    std::string_view contents,
    std::string_view label) {
    if (path.empty() || path.filename().empty()) {
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_argument,
            std::string(label) + " path must name a file"));
    }
    const auto parent = path.parent_path();
    std::error_code parent_error;
    if (!parent.empty() &&
        (!std::filesystem::exists(parent, parent_error) || parent_error ||
         !std::filesystem::is_directory(parent, parent_error) || parent_error)) {
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            std::string(label) + " parent directory is unavailable"));
    }
    std::error_code exists_error;
    if (std::filesystem::exists(path, exists_error) || exists_error) {
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_state,
            std::string(label) + " refuses to overwrite an existing file"));
    }

    auto temporary = path;
    temporary += ".cartographer-tmp";
    std::error_code temporary_error;
    if (std::filesystem::exists(temporary, temporary_error) || temporary_error) {
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_state,
            std::string(label) + " temporary output already exists"));
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "unable to open " + std::string(label) + " temporary output"));
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "failed while writing " + std::string(label) + " temporary output"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "failed while closing " + std::string(label) + " temporary output"));
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Result<void>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::io_error,
            "atomic " + std::string(label) + " replacement failed"));
    }
    return Result<void>::success();
}

Result<carto::scientific::FunctionalGenomeExchange> load_functional_genome_exchange(
    const std::filesystem::path& path) {
    const auto contents = read_bounded_text(
        path, kFunctionalGenomeExchangeMaxBytes, "functional genome exchange");
    if (!contents) {
        return Result<carto::scientific::FunctionalGenomeExchange>::failure(contents.error());
    }
    const auto exchange = carto::scientific::FunctionalGenomeExchange::deserialize(contents.value());
    if (!exchange) {
        return Result<carto::scientific::FunctionalGenomeExchange>::failure(
            exchange.error().with_context("functional genome exchange input"));
    }
    return exchange;
}

Result<carto::scientific::AnatomyProviderCandidate> load_anatomy_provider_candidate(
    const std::filesystem::path& path) {
    const auto contents = read_bounded_text(
        path, kAnatomyProviderCandidateMaxBytes, "anatomy provider candidate");
    if (!contents) {
        return Result<carto::scientific::AnatomyProviderCandidate>::failure(contents.error());
    }
    const auto candidate = carto::scientific::AnatomyProviderCandidate::deserialize(contents.value());
    if (!candidate) {
        return Result<carto::scientific::AnatomyProviderCandidate>::failure(
            candidate.error().with_context("anatomy provider candidate input"));
    }
    return candidate;
}

Result<std::uint64_t> parse_gene_id_argument(std::string_view text) {
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0U) {
        return Result<std::uint64_t>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_argument,
            "functional genome gene id must be a non-zero unsigned integer"));
    }
    return Result<std::uint64_t>::success(value);
}

Result<double> parse_finite_double_argument(std::string_view text) {
    const std::string token(text);
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value)) {
        return Result<double>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_argument,
            "functional genome gene value must be a finite number"));
    }
    return Result<double>::success(value);
}

int demo(const std::filesystem::path& path) {
    carto::application::ApplicationSession session;
    auto created_project = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Cartographer 0.1 sample"});
    if (!created_project) {
        print_error(created_project.error());
        return EXIT_FAILURE;
    }
    auto baseline = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{path});
    if (!baseline) {
        print_error(baseline.error());
        return EXIT_FAILURE;
    }
    auto created_object = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Cube", {2.0, 2.0, 2.0}});
    if (!created_object) {
        print_error(created_object.error());
        return EXIT_FAILURE;
    }
    auto receipt = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{std::nullopt});
    if (!receipt) {
        print_error(receipt.error());
        return EXIT_FAILURE;
    }
    const auto snapshot = session.snapshot();
    std::cout << "created " << path << "\n"
              << "objects=" << snapshot.objects.size() << " revision="
              << snapshot.project_revision.value() << " journaled=true"
              << "\n";
    return EXIT_SUCCESS;
}

carto::core::Result<carto::scientific::ScientificModel> build_scientific_demo_model(
    bool include_landmarks = true) {
    using namespace carto::scientific;
    const Provenance provenance{
        "local://cartographer/scientific-demo", "2026-10-03", "CC0-1.0",
        "cartographer-cli", "2026-10-03T00:00:00Z", std::nullopt};
    const std::string fixture_source = "CARTOGRAPHER_SCIENTIFIC_DEMO_INPUT 1\n";
    const auto* fixture_bytes = reinterpret_cast<const std::uint8_t*>(fixture_source.data());
    const auto fixture_source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{fixture_bytes, fixture_source.size()});

    ScientificModel model;

    if (auto result = model.insert_parameter(Parameter{
            ParameterId{1U}, "growth-rate", "1/s", 0.5, 0.0, 1.0, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.insert_field(Field{
            FieldId{1U}, FieldKind::scalar, Association::continuous, "normalized",
            "paired-local", "stage:1", std::nullopt, std::nullopt,
            "cartographer-local-preview", std::nullopt, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    MorphologyModel morphology;
    morphology.organism_identity = "Cartographer scientific demo organism";
    morphology.body_plan = "bilateral-demo";
    morphology.axes = {"anterior-posterior", "dorsal-ventral"};
    morphology.symmetry = "bilateral";
    if (auto result = morphology.insert_structure(MorphologyStructure{
            StructureId{1U}, "appendage", std::nullopt, "left", "stage:0",
            GeometryBinding{"mesh", "mesh://scientific-demo/left"}, "paired-local",
            include_landmarks ? std::vector<LandmarkId>{LandmarkId{1U}} : std::vector<LandmarkId>{},
            "authored", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = morphology.insert_structure(MorphologyStructure{
            StructureId{2U}, "appendage", std::nullopt, "right", "stage:0",
            GeometryBinding{"mesh", "mesh://scientific-demo/right"}, "paired-local",
            include_landmarks ? std::vector<LandmarkId>{LandmarkId{2U}} : std::vector<LandmarkId>{},
            "authored", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (include_landmarks) {
        if (auto result = morphology.insert_landmark(MorphologyLandmark{
                LandmarkId{1U}, "tip", StructureId{1U}, {0.0, 0.0, 0.0}, "paired-local", provenance});
            !result) {
            return carto::core::Result<ScientificModel>::failure(result.error());
        }
        if (auto result = morphology.insert_landmark(MorphologyLandmark{
                LandmarkId{2U}, "tip", StructureId{2U}, {0.2, 0.0, 0.0}, "paired-local", provenance});
            !result) {
            return carto::core::Result<ScientificModel>::failure(result.error());
        }
    }
    if (auto result = morphology.insert_growth_domain(MorphologyGrowthDomain{
            GrowthDomainId{1U}, StructureId{2U}, FieldId{1U}, {1.0, 0.0, 0.0}, 0.5,
            {1.0, 0.5, 0.25}, 2.0, "stage:1", "stage:2", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_morphology(std::move(morphology)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    FunctionalGenome genome;
    genome.schema = "cartographer.functional-genome.v2";
    genome.developmental_program_reference = "program://scientific-demo";
    genome.lineage_reference = "lineage://scientific-demo";
    if (auto result = genome.insert_gene(FunctionalGene{
            GeneId{1U}, "growth-rate", "developmental", "continuous", 0.5, "1/s", 0.0, 1.0,
            "bounded", "inherited", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = genome.insert_gene(FunctionalGene{
            GeneId{2U}, "chemical-sensitivity", "sensory", "continuous", 0.75, "normalized",
            0.0, 1.0, "bounded", "inherited", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_functional_genome(std::move(genome)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://scientific-demo";
    program.description = "deterministic authoring fixture";
    if (auto result = program.insert_stage(DevelopmentStage{
            StageId{1U}, 0U, "formation", "always", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = program.insert_stage(DevelopmentStage{
            StageId{2U}, 1U, "sensory differentiation", "growth-rate > 0", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = program.insert_instruction(DevelopmentInstruction{
            InstructionId{1U}, StageId{1U}, "instantiate", "morphogenesis", "always",
            {ParameterId{1U}},
            {StructureId{1U}}, {}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = program.insert_instruction(DevelopmentInstruction{
            InstructionId{2U}, StageId{2U}, "differentiate", "morphogenesis", "growth-rate > 0",
            {ParameterId{1U}},
            {}, {StructureId{2U}}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_developmental_program(std::move(program)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    DevelopmentalTrace trace;
    trace.program_reference = "program://scientific-demo";
    trace.genome_reference = "program://scientific-demo";
    if (auto result = trace.insert_entry(DevelopmentTraceEntry{
            TraceId{1U}, StageId{1U}, InstructionId{1U}, {StructureId{1U}}, {},
            "receipt://scientific-demo/formation", provenance, TissueRegionId{1U}, {0.5},
            std::nullopt, std::nullopt}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = trace.insert_entry(DevelopmentTraceEntry{
            TraceId{2U}, StageId{2U}, InstructionId{2U}, {}, {StructureId{2U}},
            "receipt://scientific-demo/differentiation", provenance, TissueRegionId{1U}, {0.75},
            std::nullopt, std::nullopt}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_developmental_trace(std::move(trace)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    LineageGraph lineage;
    for (std::uint64_t generation = 0U; generation <= 100U; ++generation) {
        const LineageId lineage_id{generation + 1U};
        const std::optional<LineageId> parent = generation == 0U
            ? std::nullopt
            : std::optional<LineageId>{LineageId{generation}};
        if (auto result = lineage.insert_node(LineageNode{
                lineage_id, parent, std::nullopt, generation, std::nullopt, std::nullopt,
                provenance, {}}); !result) {
            return carto::core::Result<ScientificModel>::failure(result.error());
        }
        if (generation > 0U) {
            if (auto result = lineage.insert_mutation(MutationReceipt{
                    MutationId{generation}, LineageId{generation}, lineage_id,
                    std::nullopt, std::nullopt, 42000U + generation,
                    "deterministic-fixture-adjustment", {GeneId{1U}}, {InstructionId{2U}}, {},
                    "authored receipt; not executed", provenance}); !result) {
                return carto::core::Result<ScientificModel>::failure(result.error());
            }
        }
    }
    if (auto result = model.set_lineage_graph(std::move(lineage)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    MorphometricModel morphometrics;
    if (auto result = morphometrics.insert_signature(MorphologySignature{
            SignatureId{1U}, StructureId{1U}, "landmark-length", "paired-local", "unit-norm",
            {1.0}, {"length"}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = morphometrics.insert_signature(MorphologySignature{
            SignatureId{2U}, StructureId{2U}, "landmark-length", "paired-local", "unit-norm",
            {1.2}, {"length"}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = morphometrics.insert_comparison(MorphologyComparison{
            ComparisonId{1U}, StructureId{1U}, StructureId{2U}, "provider-euclidean", 0.2,
            1.0 / 1.2, SignatureId{1U}, SignatureId{2U}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_morphometrics(std::move(morphometrics)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    SequenceGenome sequence;
    sequence.schema = "cartographer.sequence-genome.v3";
    sequence.assembly = "scientific-demo-assembly";
    sequence.sample_reference = "sample://scientific-demo";
    sequence.source_digest = fixture_source_digest;
    if (auto result = sequence.insert_contig(SequenceContig{
            ContigId{1U}, "chr-demo", "ACGT", std::nullopt, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = sequence.insert_variant(SequenceVariant{
            VariantId{1U}, ContigId{1U}, 1U, "C", "T", "SNV", provenance, ""}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (include_landmarks) {
        if (auto result = sequence.insert_feature(SequenceFeature{
                SequenceFeatureId{1U}, ContigId{1U}, 0U, 3U, "gene", "demo", 0.75, "+",
                std::nullopt, "demo-gene", {{"Name", "demo gene"}}, provenance}); !result) {
            return carto::core::Result<ScientificModel>::failure(result.error());
        }
        if (auto result = sequence.insert_feature(SequenceFeature{
                SequenceFeatureId{2U}, ContigId{1U}, 1U, 4U, "CDS", "demo", std::nullopt, "+",
                std::uint8_t{0}, "", {{"Note", "demo annotation"}}, provenance}); !result) {
            return carto::core::Result<ScientificModel>::failure(result.error());
        }
    }
    if (auto result = model.set_sequence_genome(std::move(sequence)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    AnatomyModel anatomy;
    anatomy.schema = "cartographer.anatomy.v1";
    if (auto result = anatomy.insert_term(AnatomyTerm{
            AnatomyTermId{1U}, "demo", "appendage", "Demo appendage", "organ", std::nullopt,
            "authored fixture term", provenance, "cartographer-demo-1", "demo-release-1"}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = anatomy.insert_part(AnatomyPart{
            AnatomyPartId{1U}, StructureId{1U}, AnatomyTermId{1U}, std::nullopt, "body", provenance});
        !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = anatomy.insert_tissue_region(TissueRegion{
            TissueRegionId{1U}, "muscle", StructureId{2U}, std::nullopt,
            "density=1;elasticity=0.5", "stage:1", "stage:1",
            GeometryBinding{"surface", "mesh://scientific-demo/right"}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_anatomy(std::move(anatomy)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    FieldVisualizationModel field_visualizations;
    field_visualizations.schema = "cartographer.field-visualizations.v1";
    if (auto result = field_visualizations.insert_visualization(FieldVisualization{
            VisualizationId{1U}, FieldId{1U}, std::nullopt, "scalar", "viridis", 0.0, 1.0,
            "stage:1", true, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_field_visualizations(std::move(field_visualizations)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    ScientificImportLedger imports;
    if (auto result = imports.insert_receipt(ScientificImportReceipt{
            ImportId{1U}, "scientific-fixture", "local://cartographer/scientific-demo/input",
            "cartographer-local-preview", fixture_source_digest, 3U, 2U,
            {"fixture input only"}, {"units normalized"}, provenance,
            {{"record:1", "preserved", "field:growth-rate", "direct authored mapping"},
             {"record:2", "converted", "field:chemical-sensitivity", "normalized units"},
             {"record:3", "ignored", "", "fixture metadata only"}}}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_import_ledger(std::move(imports)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    GeneRegulatoryNetwork regulatory;
    regulatory.schema = "cartographer.gene-regulatory.v1";
    regulatory.reference = "local://cartographer/scientific-demo/regulation";
    regulatory.status = "deterministic_preview";
    regulatory.provenance = provenance;
    if (auto result = regulatory.insert_element(RegulatoryElement{
            RegulatoryElementId{1U}, GeneId{1U}, "sensor enhancer", "enhancer",
            "local://cartographer/scientific-demo/enhancer-1", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = regulatory.insert_edge(RegulatoryEdge{
            RegulatoryEdgeId{1U}, GeneId{1U}, GeneId{2U}, RegulatoryElementId{1U},
            "activation", "growth-rate > 0", StageId{1U}, provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = regulatory.insert_expression(SpatialExpression{
            ExpressionId{1U}, GeneId{2U}, std::nullopt, std::nullopt, FieldId{1U}, StageId{1U},
            "continuous_field", 0.5, "", "normalized", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_gene_regulatory_network(std::move(regulatory)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    const auto external_digest = carto::assets::Sha256Digest::from_hex(
        "1111111111111111111111111111111111111111111111111111111111111111");
    const auto expected_digest = carto::assets::Sha256Digest::from_hex(
        "2222222222222222222222222222222222222222222222222222222222222222");
    if (!external_digest || !expected_digest) {
        return carto::core::Result<ScientificModel>::failure(
            carto::core::Diagnostic{carto::core::ErrorCode::validation_failed,
                                    "invalid demo replay digest"});
    }
    ScientificReplayCapsuleModel replay;
    replay.schema = "cartographer.scientific-replay.v1";
    replay.reference = "local://cartographer/scientific-demo/replay";
    replay.status = "authoring_capsule";
    replay.provenance = provenance;
    if (auto result = replay.insert_capsule(ScientificReplayCapsule{
            ReplayId{1U}, carto::core::Revision{1U}, "not-executed-reference", "not-run",
            {42U, 7U}, {ParameterId{1U}}, {external_digest.value()}, {expected_digest.value()},
            provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_replay_capsules(std::move(replay)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    const auto source_manifest_digest = carto::assets::Sha256Digest::from_hex(
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    const auto geometry_digest = carto::assets::Sha256Digest::from_hex(
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    if (!source_manifest_digest || !geometry_digest) {
        return carto::core::Result<ScientificModel>::failure(
            carto::core::Diagnostic{carto::core::ErrorCode::validation_failed,
                                    "invalid demo anatomy candidate digest"});
    }
    AnatomyProviderManifest provider_manifest;
    provider_manifest.schema = "cartographer.anatomy-provider-manifest.v1";
    provider_manifest.provider = "cartographer-candidate-adapter";
    provider_manifest.publication_reference =
        "candidate://cartographer/scientific-demo/anatomy";
    provider_manifest.status = "candidate";
    provider_manifest.source_model_revision = carto::core::Revision{1U};
    provider_manifest.source_manifest_digest = source_manifest_digest.value();
    provider_manifest.provenance = provenance;
    if (auto result = provider_manifest.insert_binding(AnatomyProviderBinding{
            AnatomyBindingId{1U}, AnatomyPartId{1U}, "demo:appendage", "right",
            GeometryBinding{"mesh", "candidate://cartographer/scientific-demo/right"},
            "fixture-2026.10", "CC0-1.0", geometry_digest.value(), "demo-material",
            "local://cartographer/scientific-demo/edit/1", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_anatomy_provider_manifest(std::move(provider_manifest)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    PhenotypeCapabilityModel phenotype;
    phenotype.schema = "cartographer.phenotype-capabilities.v1";
    phenotype.compilation_reference = "local://cartographer/scientific-demo";
    phenotype.status = "deterministic_preview";
    phenotype.provenance = provenance;
    if (auto result = phenotype.insert_capability(PhenotypeCapability{
            CapabilityId{1U}, "chemical sensing", "sensory", "present", 0.75, "normalized",
            {StructureId{2U}}, {GeneId{2U}}, {InstructionId{2U}}, {TraceId{2U}},
            "deterministic fixture: gene 2 -> instruction 2 -> structure 2", provenance}); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    if (auto result = model.set_phenotype_capabilities(std::move(phenotype)); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }

    if (auto result = model.validate(); !result) {
        return carto::core::Result<ScientificModel>::failure(result.error());
    }
    return carto::core::Result<ScientificModel>::success(std::move(model));
}

int scientific_demo(const std::filesystem::path& path) {
    carto::project::ProjectDocument document;
    std::filesystem::path journal_path = path;
    journal_path += ".journal";
    if (std::filesystem::exists(path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: scientific demo refuses to overwrite an existing project or journal: "
                  << path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "scientific-demo");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    auto model = build_scientific_demo_model(true);
    if (!model) {
        print_error(model.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model.value())); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto committed = transaction.value().commit("scientific.demo.bind");
    if (!committed) {
        print_error(committed.error());
        return EXIT_FAILURE;
    }
    const auto saved = document.save_atomic(path);
    if (!saved) {
        print_error(saved.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(path);
    if (!reopened) {
        print_error(reopened.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = reopened.value().scientific_model();
    const auto bilateral = scientific.morphology()->preview_bilateral_landmarks(
        carto::scientific::StructureId{1U}, carto::scientific::StructureId{2U});
    const auto signature = scientific.morphometrics()->preview_signature_comparison(
        carto::scientific::SignatureId{1U}, carto::scientific::SignatureId{2U});
    const auto impact = scientific.build_developmental_instruction_impact(
        carto::scientific::InstructionId{2U});
    const auto capability = scientific.phenotype_capabilities()->trace(
        carto::scientific::CapabilityId{1U});
    if (!bilateral || !signature || !impact || !capability) {
        if (!bilateral) print_error(bilateral.error());
        if (!signature) print_error(signature.error());
        if (!impact) print_error(impact.error());
        if (!capability) print_error(capability.error());
        return EXIT_FAILURE;
    }
    std::cout << "created " << path << "\n"
              << "scientific_revision=" << reopened.value().revision().value()
              << " structures=" << scientific.morphology()->structures().size()
              << " genes=" << scientific.functional_genome()->genes().size()
              << " variants=" << scientific.sequence_genome()->variants().size()
              << " landmark_rms=" << bilateral.value().rms_distance
              << " signature_distance=" << signature.value().distance
              << " impacted_structures=" << impact.value().observed_modifies.size()
              << " lineage_generations=" << scientific.lineage_graph()->nodes().size()
              << " lineage_mutations=" << scientific.lineage_graph()->mutations().size()
              << " capability=" << capability.value().name
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int scientific_reference_demo(const std::filesystem::path& path) {
    carto::project::ProjectDocument document;
    std::filesystem::path journal_path = path;
    journal_path += ".journal";
    if (std::filesystem::exists(path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: scientific reference demo refuses to overwrite an existing project or journal: "
                  << path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "scientific-reference-demo");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    auto model = build_scientific_demo_model(false);
    if (!model) {
        print_error(model.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model.value())); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.reference_demo.bind"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(path);
    if (!reopened || !reopened.value().scientific_model().morphology().has_value() ||
        !reopened.value().scientific_model().morphology()->landmarks().empty()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: scientific reference demo reopened with landmark records\n";
        return EXIT_FAILURE;
    }
    std::cout << "created " << path
              << " structures=" << reopened.value().scientific_model().morphology()->structures().size()
              << " landmarks=0 journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int validate(const std::filesystem::path& path) {
    auto document = carto::project::ProjectDocument::load(path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    std::cout << "valid " << path << "\n"
              << "schema=" << document.value().schema_version() << " objects="
              << document.value().scene().size() << " meshes=" << document.value().meshes().size()
              << " revision=" << document.value().revision().value() << "\n";
    return EXIT_SUCCESS;
}

int inspect_scientific(const std::filesystem::path& path) {
    auto document = carto::project::ProjectDocument::load(path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }

    const auto& scientific = document.value().scientific_model();
    std::cout << "scientific " << path << '\n'
              << "revision=" << document.value().revision().value()
              << " regions=" << scientific.regions().size()
              << " boundaries=" << scientific.boundaries().size()
              << " interfaces=" << scientific.interfaces().size()
              << " fields=" << scientific.fields().size()
              << " studies=" << scientific.studies().size()
              << " results=" << scientific.result_sets().size() << '\n';

    if (scientific.morphology().has_value()) {
        const auto& morphology = scientific.morphology().value();
        std::cout << "morphology=present organism=" << morphology.organism_identity
                  << " body_plan=" << morphology.body_plan
                  << " symmetry=" << morphology.symmetry
                  << " structures=" << morphology.structures().size()
                  << " attachments=" << morphology.attachments().size()
                  << " landmarks=" << morphology.landmarks().size()
                  << " growth_domains=" << morphology.growth_domains().size() << '\n';
    } else {
        std::cout << "morphology=absent\n";
    }

    if (scientific.functional_genome().has_value()) {
        const auto& genome = scientific.functional_genome().value();
        std::cout << "functional_genome=present schema=" << genome.schema
                  << " genes=" << genome.genes().size()
                  << " developmental_program=" << genome.developmental_program_reference
                  << " lineage=" << (genome.lineage_reference.empty() ? "absent" : "present")
                  << " source_digest=" << (genome.source_digest.has_value() ? "present" : "absent")
                  << '\n';
    } else {
        std::cout << "functional_genome=absent\n";
    }

    if (scientific.sequence_genome().has_value()) {
        const auto& genome = scientific.sequence_genome().value();
        std::cout << "sequence_genome=present assembly=" << genome.assembly
                  << " contigs=" << genome.contigs().size()
                  << " variants=" << genome.variants().size()
                  << " source_digest=" << (genome.source_digest.has_value() ? "present" : "absent")
                  << '\n';
    } else {
        std::cout << "sequence_genome=absent\n";
    }

    if (scientific.developmental_program().has_value()) {
        const auto timeline = scientific.build_developmental_timeline();
        if (timeline) {
            std::cout << "development=present stages="
                      << scientific.developmental_program()->stages().size()
                      << " instructions="
                      << scientific.developmental_program()->instructions().size()
                      << " timeline_steps=" << timeline.value().size() << '\n';
        } else {
            std::cout << "development=present timeline=blocked reason="
                      << timeline.error().message << '\n';
        }
    } else {
        std::cout << "development=absent\n";
    }

    if (scientific.developmental_trace().has_value()) {
        std::cout << "development_trace=present entries="
                  << scientific.developmental_trace()->entries().size() << '\n';
    } else {
        std::cout << "development_trace=absent\n";
    }

    if (scientific.lineage_graph().has_value()) {
        const auto& lineage = scientific.lineage_graph().value();
        std::cout << "lineage=present nodes=" << lineage.nodes().size()
                  << " mutations=" << lineage.mutations().size()
                  << " recombinations=" << lineage.recombinations().size() << '\n';
    } else {
        std::cout << "lineage=absent\n";
    }

    if (scientific.morphometrics().has_value()) {
        const auto& morphometrics = scientific.morphometrics().value();
        std::cout << "morphometrics=present observations=" << morphometrics.observations().size()
                  << " signatures=" << morphometrics.signatures().size()
                  << " comparisons=" << morphometrics.comparisons().size() << '\n';
    } else {
        std::cout << "morphometrics=absent\n";
    }

    if (scientific.anatomy().has_value()) {
        const auto& anatomy = scientific.anatomy().value();
        std::cout << "anatomy=present terms=" << anatomy.terms().size()
                  << " parts=" << anatomy.parts().size()
                  << " tissue_regions=" << anatomy.tissue_regions().size()
                  << " relations=" << anatomy.relations().size();
        if (!anatomy.terms().empty()) {
            const auto& term = anatomy.terms().begin()->second;
            std::cout << " ontology_version="
                      << (term.ontology_version.empty() ? "absent" : term.ontology_version)
                      << " source_release="
                      << (term.source_release.empty() ? "absent" : term.source_release);
        }
        std::cout << '\n';
    } else {
        std::cout << "anatomy=absent\n";
    }

    if (scientific.field_visualizations().has_value()) {
        std::cout << "field_visualizations=present descriptors="
                  << scientific.field_visualizations()->visualizations().size()
                  << " cpu_preview=scalar/vector/slice/contour/isosurface/volume/probe/streamline-gray-grayscale-viridis\n";
    } else {
        std::cout << "field_visualizations=absent\n";
    }

    if (scientific.import_ledger().has_value()) {
        std::size_t preserved = 0U;
        std::size_t converted = 0U;
        std::size_t ignored = 0U;
        std::size_t unsupported = 0U;
        std::size_t ambiguous = 0U;
        std::size_t repaired = 0U;
        for (const auto& [id, receipt] : scientific.import_ledger()->receipts()) {
            static_cast<void>(id);
            for (const auto& disposition : receipt.preservation_report) {
                if (disposition.disposition == "preserved") ++preserved;
                else if (disposition.disposition == "converted") ++converted;
                else if (disposition.disposition == "ignored") ++ignored;
                else if (disposition.disposition == "unsupported") ++unsupported;
                else if (disposition.disposition == "ambiguous") ++ambiguous;
                else if (disposition.disposition == "repaired") ++repaired;
            }
        }
        std::cout << "imports=present receipts=" << scientific.import_ledger()->receipts().size()
                  << " preserved=" << preserved << " converted=" << converted
                  << " ignored=" << ignored << " unsupported=" << unsupported
                  << " ambiguous=" << ambiguous << " repaired=" << repaired << '\n';
    } else {
        std::cout << "imports=absent\n";
    }

    if (scientific.phenotype_capabilities().has_value()) {
        const auto& phenotype = scientific.phenotype_capabilities().value();
        std::cout << "phenotype=present status=" << phenotype.status
                  << " capabilities=" << phenotype.capabilities().size()
                  << " trace_queries=forward-reverse" << '\n';
    } else {
        std::cout << "phenotype=absent\n";
    }

    if (scientific.gene_regulatory_network().has_value()) {
        const auto& network = scientific.gene_regulatory_network().value();
        std::cout << "regulatory_network=present elements=" << network.elements().size()
                  << " edges=" << network.edges().size()
                  << " expressions=" << network.expressions().size() << '\n';
    } else {
        std::cout << "regulatory_network=absent\n";
    }

    if (scientific.replay_capsules().has_value()) {
        const auto& capsules = scientific.replay_capsules().value();
        std::cout << "replay_capsules=present status=" << capsules.status
                  << " capsules=" << capsules.capsules().size() << '\n';
    } else {
        std::cout << "replay_capsules=absent\n";
    }

    if (scientific.anatomy_provider_manifest().has_value()) {
        const auto& manifest = scientific.anatomy_provider_manifest().value();
        const auto candidate = scientific.validate_anatomy_provider_candidate(
            document.value().revision());
        std::cout << "provider_manifest=present provider=" << manifest.provider
                  << " status=" << manifest.status
                  << " bindings=" << manifest.bindings().size()
                  << " source_revision="
                  << (manifest.source_model_revision.has_value() ? "present" : "absent")
                  << " candidate_gate=" << (candidate ? "pass" : "blocked");
        if (!candidate) std::cout << " reason=" << candidate.error().message;
        std::cout << '\n';
    } else {
        std::cout << "provider_manifest=absent\n";
    }

    return EXIT_SUCCESS;
}

int export_sequence_fasta(
    const std::filesystem::path& project_path,
    const std::filesystem::path& fasta_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.sequence_genome().has_value()) {
        std::cerr << "error: project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.sequence_genome()->export_fasta();
    if (!exported) {
        print_error(exported.error().with_context("sequence FASTA export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            fasta_path, exported.value().text, "sequence FASTA"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << fasta_path
              << " contigs=" << scientific.sequence_genome()->contigs().size()
              << " variants_omitted=" << scientific.sequence_genome()->variants().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_sequence_fasta(
    const std::filesystem::path& fasta_path,
    std::string_view assembly,
    std::string_view sample_reference,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        fasta_path, kSequenceGenomeFastaMaxBytes, "sequence FASTA");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = fasta_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/fasta/" + source_name,
        "external-fasta",
        "unspecified",
        "cartographer-fasta-adapter",
        "",
        source_digest};
    const auto sequence = carto::scientific::SequenceGenome::import_fasta(
        contents.value(), std::string(assembly), std::string(sample_reference), provenance,
        source_digest);
    if (!sequence) {
        print_error(sequence.error().with_context("sequence FASTA import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: sequence FASTA import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model;
    if (auto result = model.set_sequence_genome(sequence.value()); !result) {
        print_error(result.error().with_context("sequence FASTA import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger;
    const auto contig_count = static_cast<std::uint64_t>(sequence.value().contigs().size());
    carto::scientific::ScientificImportReceipt receipt{
            carto::scientific::ImportId{1U}, "FASTA", source_name,
            "cartographer-fasta-adapter", source_digest, contig_count, contig_count,
            {"FASTA does not carry Cartographer assembly, sample, source-digest, or per-record provenance metadata"},
            {"FASTA does not carry explicit sequence variants"}, provenance, {}};
    for (const auto& [id, contig] : sequence.value().contigs()) {
        static_cast<void>(contig);
        receipt.preservation_report.push_back({
            "record:" + std::to_string(id.value), "preserved",
            "sequence:contig:" + std::to_string(id.value),
            "uppercase-IUPAC sequence and FASTA header admitted"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("sequence FASTA import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("sequence FASTA import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "sequence-fasta-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.sequence_genome.import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().sequence_genome().has_value() ||
        reopened.value().scientific_model().sequence_genome()->serialize() !=
            sequence.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported sequence genome failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << fasta_path << " -> " << project_path
              << " contigs=" << sequence.value().contigs().size()
              << " variants=0 source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_sequence_vcf(
    const std::filesystem::path& project_path,
    const std::filesystem::path& vcf_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.sequence_genome().has_value()) {
        std::cerr << "error: project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.sequence_genome()->export_vcf();
    if (!exported) {
        print_error(exported.error().with_context("sequence VCF export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            vcf_path, exported.value().text, "sequence VCF"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << vcf_path
              << " contigs=" << scientific.sequence_genome()->contigs().size()
              << " variants=" << scientific.sequence_genome()->variants().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_sequence_vcf(
    const std::filesystem::path& vcf_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        vcf_path, kSequenceGenomeVcfMaxBytes, "sequence VCF");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("VCF reference project"));
        return EXIT_FAILURE;
    }
    if (!reference.value().scientific_model().sequence_genome().has_value()) {
        std::cerr << "error: reference project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = vcf_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/vcf/" + source_name,
        "external-vcf",
        "unspecified",
        "cartographer-vcf-adapter",
        "",
        source_digest};
    const auto sequence = carto::scientific::SequenceGenome::import_vcf(
        contents.value(), *reference.value().scientific_model().sequence_genome(), provenance);
    if (!sequence) {
        print_error(sequence.error().with_context("sequence VCF import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: sequence VCF import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_sequence_genome(sequence.value()); !result) {
        print_error(result.error().with_context("sequence VCF import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, receipt] : ledger.receipts()) {
        static_cast<void>(receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "VCF", source_name,
        "cartographer-vcf-adapter", source_digest,
        static_cast<std::uint64_t>(sequence.value().variants().size()),
        static_cast<std::uint64_t>(sequence.value().variants().size()),
        {"VCF import was validated against the supplied reference digest, assembly, sample, and coordinate system"},
        {"VCF does not carry full reference contig sequence bytes or per-record Cartographer provenance"},
        provenance, {}};
    for (const auto& [id, variant] : sequence.value().variants()) {
        static_cast<void>(variant);
        receipt.preservation_report.push_back({
            "variant:" + std::to_string(id.value), "preserved",
            "sequence:variant:" + std::to_string(id.value),
            "reference-relative allele and coordinate admitted"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("sequence VCF import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("sequence VCF import ledger"));
        return EXIT_FAILURE;
    }

    // The reference project is read-only input. The imported variant genome
    // is admitted as a new authoring document so its journal starts at the
    // normal genesis revision rather than inheriting an unrelated journal
    // tail from the reference project.
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "sequence-vcf-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.sequence_genome.vcf_import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().sequence_genome().has_value() ||
        reopened.value().scientific_model().sequence_genome()->serialize() !=
            sequence.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported sequence VCF failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << vcf_path << " against " << reference_project_path
              << " -> " << project_path
              << " variants=" << sequence.value().variants().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_sequence_gff3(
    const std::filesystem::path& project_path,
    const std::filesystem::path& gff3_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.sequence_genome().has_value()) {
        std::cerr << "error: project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.sequence_genome()->export_gff3();
    if (!exported) {
        print_error(exported.error().with_context("sequence GFF3 export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            gff3_path, exported.value().text, "sequence GFF3"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << gff3_path
              << " contigs=" << scientific.sequence_genome()->contigs().size()
              << " features=" << scientific.sequence_genome()->features().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_sequence_gff3(
    const std::filesystem::path& gff3_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        gff3_path, kSequenceGenomeGff3MaxBytes, "sequence GFF3");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("GFF3 reference project"));
        return EXIT_FAILURE;
    }
    if (!reference.value().scientific_model().sequence_genome().has_value()) {
        std::cerr << "error: reference project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = gff3_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/gff3/" + source_name,
        "external-gff3",
        "unspecified",
        "cartographer-gff3-adapter",
        "",
        source_digest};
    const auto sequence = carto::scientific::SequenceGenome::import_gff3(
        contents.value(), *reference.value().scientific_model().sequence_genome(), provenance);
    if (!sequence) {
        print_error(sequence.error().with_context("sequence GFF3 import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: sequence GFF3 import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_sequence_genome(sequence.value()); !result) {
        print_error(result.error().with_context("sequence GFF3 import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, existing_receipt] : ledger.receipts()) {
        static_cast<void>(existing_receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "GFF3", source_name,
        "cartographer-gff3-adapter", source_digest,
        static_cast<std::uint64_t>(sequence.value().features().size()),
        static_cast<std::uint64_t>(sequence.value().features().size()),
        {"GFF3 intervals were admitted only after reference digest, coordinate, and contig validation"},
        {"GFF3 annotations do not imply functional-genome, developmental, phenotype, or runtime semantics",
         "Per-feature Cartographer provenance is replaced by the caller provenance"},
        provenance, {}};
    for (const auto& [id, feature] : sequence.value().features()) {
        static_cast<void>(feature);
        receipt.preservation_report.push_back({
            "feature:" + std::to_string(id.value), "preserved",
            "sequence:feature:" + std::to_string(id.value),
            "GFF3 interval, source/type, bounded attributes, and stable identity admitted"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("sequence GFF3 import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("sequence GFF3 import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "sequence-gff3-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.sequence_genome.gff3_import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().sequence_genome().has_value() ||
        reopened.value().scientific_model().sequence_genome()->serialize() !=
            sequence.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported sequence GFF3 failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << gff3_path << " against " << reference_project_path
              << " -> " << project_path
              << " features=" << sequence.value().features().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_sequence_bed6(
    const std::filesystem::path& project_path,
    const std::filesystem::path& bed6_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.sequence_genome().has_value()) {
        std::cerr << "error: project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.sequence_genome()->export_bed6();
    if (!exported) {
        print_error(exported.error().with_context("sequence BED6 export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            bed6_path, exported.value().text, "sequence BED6"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << bed6_path
              << " contigs=" << scientific.sequence_genome()->contigs().size()
              << " features=" << scientific.sequence_genome()->features().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_sequence_bed6(
    const std::filesystem::path& bed6_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        bed6_path, kSequenceGenomeBed6MaxBytes, "sequence BED6");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("BED6 reference project"));
        return EXIT_FAILURE;
    }
    if (!reference.value().scientific_model().sequence_genome().has_value()) {
        std::cerr << "error: reference project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = bed6_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/bed6/" + source_name,
        "external-bed6",
        "unspecified",
        "cartographer-bed6-adapter",
        "",
        source_digest};
    const auto sequence = carto::scientific::SequenceGenome::import_bed6(
        contents.value(), *reference.value().scientific_model().sequence_genome(), provenance);
    if (!sequence) {
        print_error(sequence.error().with_context("sequence BED6 import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: sequence BED6 import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_sequence_genome(sequence.value()); !result) {
        print_error(result.error().with_context("sequence BED6 import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, existing_receipt] : ledger.receipts()) {
        static_cast<void>(existing_receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "BED6", source_name,
        "cartographer-bed6-adapter", source_digest,
        static_cast<std::uint64_t>(sequence.value().features().size()),
        static_cast<std::uint64_t>(sequence.value().features().size()),
        {"BED6 intervals were admitted only after reference digest, coordinate, and contig validation"},
        {"BED6 omits feature type, source, phase, and attributes; score is admitted on BED's integer scale",
         "BED6 annotations do not imply functional-genome, developmental, phenotype, or runtime semantics",
         "Per-feature Cartographer provenance is replaced by the caller provenance"},
        provenance, {}};
    for (const auto& [id, feature] : sequence.value().features()) {
        static_cast<void>(feature);
        receipt.preservation_report.push_back({
            "feature:" + std::to_string(id.value), "converted",
            "sequence:feature:" + std::to_string(id.value),
            "BED6 interval, stable name identity, integer score, and strand admitted"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("sequence BED6 import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("sequence BED6 import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "sequence-bed6-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.sequence_genome.bed6_import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().sequence_genome().has_value() ||
        reopened.value().scientific_model().sequence_genome()->serialize() !=
            sequence.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported sequence BED6 failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << bed6_path << " against " << reference_project_path
              << " -> " << project_path
              << " features=" << sequence.value().features().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_sequence_gtf(
    const std::filesystem::path& project_path,
    const std::filesystem::path& gtf_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.sequence_genome().has_value()) {
        std::cerr << "error: project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.sequence_genome()->export_gtf();
    if (!exported) {
        print_error(exported.error().with_context("sequence GTF export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            gtf_path, exported.value().text, "sequence GTF"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << gtf_path
              << " contigs=" << scientific.sequence_genome()->contigs().size()
              << " features=" << scientific.sequence_genome()->features().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_sequence_gtf(
    const std::filesystem::path& gtf_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        gtf_path, kSequenceGenomeGtfMaxBytes, "sequence GTF");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("GTF reference project"));
        return EXIT_FAILURE;
    }
    if (!reference.value().scientific_model().sequence_genome().has_value()) {
        std::cerr << "error: reference project has no sequence genome\n";
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = gtf_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/gtf/" + source_name,
        "external-gtf",
        "unspecified",
        "cartographer-gtf-adapter",
        "",
        source_digest};
    const auto sequence = carto::scientific::SequenceGenome::import_gtf(
        contents.value(), *reference.value().scientific_model().sequence_genome(), provenance);
    if (!sequence) {
        print_error(sequence.error().with_context("sequence GTF import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: sequence GTF import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_sequence_genome(sequence.value()); !result) {
        print_error(result.error().with_context("sequence GTF import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, existing_receipt] : ledger.receipts()) {
        static_cast<void>(existing_receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "GTF", source_name,
        "cartographer-gtf-adapter", source_digest,
        static_cast<std::uint64_t>(sequence.value().features().size()),
        static_cast<std::uint64_t>(sequence.value().features().size()),
        {"GTF intervals were admitted only after reference digest, coordinate, contig, and quoted-attribute validation"},
        {"GTF omits per-feature Cartographer provenance; import uses the caller provenance",
         "GTF annotations do not imply functional-genome, developmental, phenotype, or runtime semantics"},
        provenance, {}};
    for (const auto& [id, feature] : sequence.value().features()) {
        static_cast<void>(feature);
        receipt.preservation_report.push_back({
            "feature:" + std::to_string(id.value), "preserved",
            "sequence:feature:" + std::to_string(id.value),
            "GTF interval, supported nine-column fields, quoted attributes, and stable identity admitted"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("sequence GTF import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("sequence GTF import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "sequence-gtf-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.sequence_genome.gtf_import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().sequence_genome().has_value() ||
        reopened.value().scientific_model().sequence_genome()->serialize() !=
            sequence.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported sequence GTF failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << gtf_path << " against " << reference_project_path
              << " -> " << project_path
              << " features=" << sequence.value().features().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_lineage_newick(
    const std::filesystem::path& project_path,
    const std::filesystem::path& newick_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.lineage_graph().has_value()) {
        std::cerr << "error: project has no lineage graph\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.lineage_graph()->export_newick();
    if (!exported) {
        print_error(exported.error().with_context("lineage Newick export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            newick_path, exported.value().text, "lineage Newick"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << newick_path
              << " nodes=" << scientific.lineage_graph()->nodes().size()
              << " mutations=" << scientific.lineage_graph()->mutations().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_lineage_newick(
    const std::filesystem::path& newick_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        newick_path, kLineageNewickMaxBytes, "lineage Newick");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("Newick reference project"));
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = newick_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/newick/" + source_name,
        "external-newick",
        "unspecified",
        "cartographer-newick-adapter",
        "",
        source_digest};
    const auto lineage = carto::scientific::LineageGraph::import_newick(
        contents.value(), provenance);
    if (!lineage) {
        print_error(lineage.error().with_context("lineage Newick import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: lineage Newick import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_lineage_graph(lineage.value()); !result) {
        print_error(result.error().with_context("lineage Newick import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, existing_receipt] : ledger.receipts()) {
        static_cast<void>(existing_receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "Newick", source_name,
        "cartographer-newick-adapter", source_digest,
        static_cast<std::uint64_t>(lineage.value().nodes().size()),
        static_cast<std::uint64_t>(lineage.value().nodes().size()),
        {"rooted single-parent topology and explicit Cartographer labels were admitted after strict parsing"},
        {"Newick omits mutation/recombination receipts, genome/morphology digests, and per-node provenance",
         "generic external labels receive deterministic path-derived local IDs",
         "Newick lineage is authoring/reference data and does not imply phenotype, development, or runtime behavior"},
        provenance, {}};
    for (const auto& [id, node] : lineage.value().nodes()) {
        receipt.preservation_report.push_back({
            "lineage:" + std::to_string(id.value),
            node.source_identifier.empty() ? "converted" : "preserved",
            "lineage:node:" + std::to_string(id.value),
            node.source_identifier.empty()
                ? "Newick node admitted with native identity and path-derived or unlabeled source semantics"
                : "Newick node label admitted with stable identity, generation, and source identifier"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("lineage Newick import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("lineage Newick import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "lineage-newick-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.lineage_graph.newick_import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().lineage_graph().has_value() ||
        reopened.value().scientific_model().lineage_graph()->serialize() !=
            lineage.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported lineage Newick failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << newick_path << " against " << reference_project_path
              << " -> " << project_path
              << " nodes=" << lineage.value().nodes().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_anatomy_ontology_table(
    const std::filesystem::path& project_path,
    const std::filesystem::path& ontology_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.anatomy().has_value()) {
        std::cerr << "error: project has no anatomy model\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.anatomy()->export_ontology_table();
    if (!exported) {
        print_error(exported.error().with_context("anatomy ontology-table export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            ontology_path, exported.value().text, "anatomy ontology table"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << ontology_path
              << " terms=" << scientific.anatomy()->terms().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_anatomy_ontology_table(
    const std::filesystem::path& ontology_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        ontology_path, kAnatomyOntologyTableMaxBytes, "anatomy ontology table");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("ontology-table reference project"));
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = ontology_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/ontology-table/" + source_name,
        "external-ontology-table",
        "unspecified",
        "cartographer-ontology-table-adapter",
        "",
        source_digest};
    carto::scientific::AnatomyModel reference_anatomy;
    if (reference.value().scientific_model().anatomy().has_value()) {
        reference_anatomy = reference.value().scientific_model().anatomy().value();
    }
    const auto anatomy = carto::scientific::AnatomyModel::import_ontology_table(
        contents.value(), std::move(reference_anatomy), provenance);
    if (!anatomy) {
        print_error(anatomy.error().with_context("anatomy ontology-table import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: anatomy ontology-table import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_anatomy(anatomy.value()); !result) {
        print_error(result.error().with_context("anatomy ontology-table import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, existing_receipt] : ledger.receipts()) {
        static_cast<void>(existing_receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "OntologyTable", source_name,
        "cartographer-ontology-table-adapter", source_digest,
        static_cast<std::uint64_t>(anatomy.value().terms().size()),
        static_cast<std::uint64_t>(anatomy.value().terms().size()),
        {"ontology term hierarchy, labels, definitions, categories, and release metadata were admitted after strict validation"},
        {"ontology-table import omits parts, tissue regions, relations, geometry, and per-term provenance",
         "ontology terms remain semantic authoring data and do not imply tissue, phenotype, or runtime behavior"},
        provenance, {}};
    for (const auto& [id, term] : anatomy.value().terms()) {
        receipt.preservation_report.push_back({
            "term:" + std::to_string(id.value), "preserved",
            "anatomy:term:" + std::to_string(id.value),
            "ontology identity, hierarchy, label/category, definition, and release metadata admitted"});
        static_cast<void>(term);
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("anatomy ontology-table import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("anatomy ontology-table import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "anatomy-ontology-table-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.anatomy.ontology_table_import");
        !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().anatomy().has_value() ||
        reopened.value().scientific_model().anatomy()->serialize() != anatomy.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported anatomy ontology table failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << ontology_path << " against " << reference_project_path
              << " -> " << project_path
              << " terms=" << anatomy.value().terms().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_landmark_set(
    const std::filesystem::path& project_path,
    const std::filesystem::path& landmark_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.morphology().has_value()) {
        std::cerr << "error: project has no morphology model\n";
        return EXIT_FAILURE;
    }
    const auto exported = scientific.morphology()->export_landmark_set();
    if (!exported) {
        print_error(exported.error().with_context("morphology landmark-set export"));
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            landmark_path, exported.value().text, "morphology landmark-set"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << landmark_path
              << " landmarks=" << scientific.morphology()->landmarks().size()
              << " structures=" << scientific.morphology()->structures().size() << '\n';
    for (const auto& note : exported.value().feature_loss_notes) {
        std::cout << "warning: " << note << '\n';
    }
    return EXIT_SUCCESS;
}

int import_landmark_set(
    const std::filesystem::path& landmark_path,
    const std::filesystem::path& reference_project_path,
    const std::filesystem::path& project_path) {
    const auto contents = read_bounded_text(
        landmark_path, kMorphologyLandmarkSetMaxBytes, "morphology landmark-set");
    if (!contents) {
        print_error(contents.error());
        return EXIT_FAILURE;
    }
    const auto reference = carto::project::ProjectDocument::load(reference_project_path);
    if (!reference) {
        print_error(reference.error().with_context("landmark-set reference project"));
        return EXIT_FAILURE;
    }
    if (!reference.value().scientific_model().morphology().has_value()) {
        std::cerr << "error: reference project has no morphology model\n";
        return EXIT_FAILURE;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(contents.value().data());
    const auto source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, contents.value().size()});
    const std::string source_name = landmark_path.filename().generic_string();
    const carto::scientific::Provenance provenance{
        "source://cartographer/landmarks/" + source_name,
        "external-landmark-set",
        "unspecified",
        "cartographer-landmark-adapter",
        "",
        source_digest};
    const auto morphology = carto::scientific::MorphologyModel::import_landmark_set(
        contents.value(), *reference.value().scientific_model().morphology(), provenance);
    if (!morphology) {
        print_error(morphology.error().with_context("morphology landmark-set import"));
        return EXIT_FAILURE;
    }

    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: morphology landmark-set import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model = reference.value().scientific_model();
    if (auto result = model.set_morphology(morphology.value()); !result) {
        print_error(result.error().with_context("morphology landmark-set import model"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger =
        model.import_ledger().has_value()
        ? model.import_ledger().value()
        : carto::scientific::ScientificImportLedger{};
    std::uint64_t next_import_id = 1U;
    for (const auto& [id, existing_receipt] : ledger.receipts()) {
        static_cast<void>(existing_receipt);
        if (id.value == std::numeric_limits<std::uint64_t>::max()) {
            std::cerr << "error: reference import ledger has no available receipt id\n";
            return EXIT_FAILURE;
        }
        next_import_id = std::max(next_import_id, id.value + 1U);
    }
    carto::scientific::ScientificImportReceipt receipt{
        carto::scientific::ImportId{next_import_id}, "LANDMARK_TSV", source_name,
        "cartographer-landmark-adapter", source_digest,
        static_cast<std::uint64_t>(morphology.value().landmarks().size()),
        static_cast<std::uint64_t>(morphology.value().landmarks().size()),
        {"Landmarks were admitted only after structure-reference, coordinate-frame, and morphology-digest validation"},
        {"Landmark exchange does not carry per-landmark provenance or provider-specific geometry attachments"},
        provenance, {}};
    for (const auto& [id, landmark] : morphology.value().landmarks()) {
        static_cast<void>(landmark);
        receipt.preservation_report.push_back({
            "landmark:" + std::to_string(id.value), "preserved",
            "morphology:landmark:" + std::to_string(id.value),
            "stable identity, structure ownership, coordinate, and frame admitted"});
    }
    if (auto result = ledger.insert_receipt(std::move(receipt)); !result) {
        print_error(result.error().with_context("morphology landmark-set import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("morphology landmark-set import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "morphology-landmark-set-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.morphology.landmark_set_import");
        !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = document.save_atomic(project_path); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().morphology().has_value() ||
        reopened.value().scientific_model().morphology()->serialize() !=
            morphology.value().serialize() ||
        !reopened.value().scientific_model().import_ledger().has_value()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported morphology landmark-set failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << landmark_path << " against " << reference_project_path
              << " -> " << project_path
              << " landmarks=" << morphology.value().landmarks().size()
              << " source_digest=" << source_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int inspect_functional_genome_exchange(const std::filesystem::path& path) {
    const auto exchange = load_functional_genome_exchange(path);
    if (!exchange) {
        print_error(exchange.error());
        return EXIT_FAILURE;
    }
    const auto& value = exchange.value();
    std::size_t preserved = 0U;
    std::size_t converted = 0U;
    std::size_t ignored = 0U;
    std::size_t unsupported = 0U;
    std::size_t ambiguous = 0U;
    std::size_t repaired = 0U;
    for (const auto& disposition : value.import_receipt.preservation_report) {
        if (disposition.disposition == "preserved") ++preserved;
        else if (disposition.disposition == "converted") ++converted;
        else if (disposition.disposition == "ignored") ++ignored;
        else if (disposition.disposition == "unsupported") ++unsupported;
        else if (disposition.disposition == "ambiguous") ++ambiguous;
        else if (disposition.disposition == "repaired") ++repaired;
    }
    std::cout << "functional_genome_exchange " << path << '\n'
              << "format=" << value.import_receipt.format
              << " provider=" << value.import_receipt.provider
              << " source=" << value.import_receipt.source_reference
              << " genes=" << value.genome.genes().size()
              << " semantic_digest=" << value.semantic_digest.hex()
              << " source_digest=" << value.import_receipt.source_digest->hex() << '\n'
              << "preservation=preserved:" << preserved
              << " converted:" << converted
              << " ignored:" << ignored
              << " unsupported:" << unsupported
              << " ambiguous:" << ambiguous
              << " repaired:" << repaired << '\n';
    return EXIT_SUCCESS;
}

int inspect_anatomy_provider_candidate(const std::filesystem::path& path) {
    const auto candidate = load_anatomy_provider_candidate(path);
    if (!candidate) {
        print_error(candidate.error());
        return EXIT_FAILURE;
    }
    const auto& value = candidate.value();
    std::cout << "anatomy_provider_candidate " << path << '\n'
              << "provider=" << value.manifest.provider
              << " publication=" << value.manifest.publication_reference
              << " status=" << value.manifest.status
              << " source_revision=" << value.source_model_revision.value()
              << " bindings=" << value.manifest.bindings().size()
              << " manifest_digest=" << value.manifest_digest.hex() << '\n';
    for (const auto& [id, binding] : value.manifest.bindings()) {
        std::cout << "binding=" << id.value
                  << " semantic_id=" << binding.semantic_id
                  << " laterality=" << binding.laterality
                  << " geometry_digest="
                  << (binding.geometry_digest.has_value() ? "present" : "absent")
                  << " tissue_material="
                  << (binding.tissue_material.empty() ? "absent" : "present") << '\n';
    }
    return EXIT_SUCCESS;
}

int export_anatomy_provider_candidate(
    const std::filesystem::path& project_path,
    const std::filesystem::path& candidate_path) {
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.anatomy_provider_manifest().has_value()) {
        std::cerr << "error: project has no anatomy provider manifest\n";
        return EXIT_FAILURE;
    }
    const auto expected_revision = document.value().revision();
    const auto candidate = scientific.build_anatomy_provider_candidate(expected_revision);
    if (!candidate) {
        print_error(candidate.error().with_context("anatomy provider candidate export"));
        return EXIT_FAILURE;
    }
    const std::string encoded = candidate.value().serialize();
    if (encoded.empty()) {
        std::cerr << "error: anatomy provider candidate serialization failed closed\n";
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            candidate_path, encoded, "anatomy provider candidate"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << candidate_path
              << " provider=" << candidate.value().manifest.provider
              << " source_revision=" << expected_revision.value()
              << " bindings=" << candidate.value().manifest.bindings().size()
              << " manifest_digest=" << candidate.value().manifest_digest.hex() << '\n';
    return EXIT_SUCCESS;
}

int edit_functional_genome_exchange(
    const std::filesystem::path& input_path,
    std::string_view gene_id_text,
    std::string_view value_text,
    const std::filesystem::path& output_path) {
    const auto gene_id = parse_gene_id_argument(gene_id_text);
    if (!gene_id) {
        print_error(gene_id.error());
        return EXIT_FAILURE;
    }
    const auto value = parse_finite_double_argument(value_text);
    if (!value) {
        print_error(value.error());
        return EXIT_FAILURE;
    }
    const auto exchange = load_functional_genome_exchange(input_path);
    if (!exchange) {
        print_error(exchange.error());
        return EXIT_FAILURE;
    }
    auto edited = exchange.value().genome;
    const auto iterator = edited.genes().find(carto::scientific::GeneId{gene_id.value()});
    if (iterator == edited.genes().end()) {
        std::cerr << "error: functional genome gene id was not found\n";
        return EXIT_FAILURE;
    }
    auto gene = iterator->second;
    gene.value = value.value();
    gene.provenance.source_reference =
        "local://cartographer/functional-genome-edit/gene/" +
        std::to_string(gene_id.value());
    gene.provenance.release = "cartographer-authoring-edit-v1";
    gene.provenance.provider = "cartographer-cli";
    gene.provenance.imported_at_utc.clear();
    gene.provenance.content_digest.reset();
    if (auto result = edited.replace_gene(std::move(gene)); !result) {
        print_error(result.error().with_context("functional genome edit"));
        return EXIT_FAILURE;
    }
    const auto edited_exchange = carto::scientific::FunctionalGenomeExchange::from_import(
        std::move(edited), exchange.value().import_receipt);
    if (!edited_exchange) {
        print_error(edited_exchange.error().with_context("functional genome edit exchange"));
        return EXIT_FAILURE;
    }
    const std::string encoded = edited_exchange.value().serialize();
    if (encoded.empty()) {
        std::cerr << "error: edited functional genome exchange serialization failed closed\n";
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            output_path, encoded, "edited functional genome exchange"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "edited " << input_path << " -> " << output_path
              << " gene=" << gene_id.value()
              << " value=" << value.value()
              << " semantic_digest=" << edited_exchange.value().semantic_digest.hex() << '\n';
    return EXIT_SUCCESS;
}

int export_functional_genome_exchange(
    const std::filesystem::path& project_path,
    const std::filesystem::path& exchange_path) {
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto& scientific = document.value().scientific_model();
    if (!scientific.functional_genome().has_value()) {
        std::cerr << "error: project has no functional genome\n";
        return EXIT_FAILURE;
    }
    if (!scientific.import_ledger().has_value() ||
        scientific.import_ledger()->receipts().size() != 1U) {
        std::cerr << "error: functional genome export requires exactly one source import receipt\n";
        return EXIT_FAILURE;
    }
    const auto& receipt = scientific.import_ledger()->receipts().begin()->second;
    const auto exchange = carto::scientific::FunctionalGenomeExchange::from_import(
        *scientific.functional_genome(), receipt);
    if (!exchange) {
        print_error(exchange.error().with_context("functional genome export"));
        return EXIT_FAILURE;
    }
    const std::string encoded = exchange.value().serialize();
    if (encoded.empty()) {
        std::cerr << "error: functional genome exchange serialization failed closed\n";
        return EXIT_FAILURE;
    }
    if (auto result = write_new_text_atomically(
            exchange_path, encoded, "functional genome exchange"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << exchange_path
              << " genes=" << exchange.value().genome.genes().size()
              << " semantic_digest=" << exchange.value().semantic_digest.hex() << '\n';
    return EXIT_SUCCESS;
}

int import_functional_genome_exchange(
    const std::filesystem::path& exchange_path,
    const std::filesystem::path& project_path) {
    const auto exchange = load_functional_genome_exchange(exchange_path);
    if (!exchange) {
        print_error(exchange.error());
        return EXIT_FAILURE;
    }
    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    if (std::filesystem::exists(project_path) || std::filesystem::exists(journal_path)) {
        std::cerr << "error: functional genome import refuses to overwrite an existing project or journal: "
                  << project_path << " / " << journal_path << '\n';
        return EXIT_FAILURE;
    }

    carto::scientific::ScientificModel model;
    if (auto result = model.set_functional_genome(exchange.value().genome); !result) {
        print_error(result.error().with_context("functional genome import"));
        return EXIT_FAILURE;
    }
    carto::scientific::ScientificImportLedger ledger;
    if (auto result = ledger.insert_receipt(exchange.value().import_receipt); !result) {
        print_error(result.error().with_context("functional genome import receipt"));
        return EXIT_FAILURE;
    }
    if (auto result = model.set_import_ledger(std::move(ledger)); !result) {
        print_error(result.error().with_context("functional genome import ledger"));
        return EXIT_FAILURE;
    }

    carto::project::ProjectDocument document;
    carto::journal::Journal journal(journal_path);
    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "functional-genome-import");
    if (!transaction) {
        print_error(transaction.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().set_scientific_model(std::move(model)); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = transaction.value().commit("scientific.functional_genome.import"); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto saved = document.save_atomic(project_path);
    if (!saved) {
        print_error(saved.error());
        return EXIT_FAILURE;
    }
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    if (!reopened || !reopened.value().scientific_model().functional_genome().has_value() ||
        reopened.value().scientific_model().functional_genome()->serialize() !=
            exchange.value().genome.serialize()) {
        if (!reopened) print_error(reopened.error());
        else std::cerr << "error: imported functional genome failed save/reopen verification\n";
        return EXIT_FAILURE;
    }
    std::cout << "imported " << exchange_path << " -> " << project_path
              << " genes=" << exchange.value().genome.genes().size()
              << " semantic_digest=" << exchange.value().semantic_digest.hex()
              << " journaled=true reopened=true\n";
    return EXIT_SUCCESS;
}

int export_obj(const std::filesystem::path& project_path, const std::filesystem::path& obj_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_obj_providers(providers); !result) {
        print_error(result.error().with_context("OBJ exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.obj"); !result) {
        print_error(result.error().with_context("OBJ exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_obj(document.value().meshes().begin()->second, obj_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << obj_path << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) {
        std::cout << "warning: " << warning << '\n';
    }
    return EXIT_SUCCESS;
}

int export_ply(const std::filesystem::path& project_path, const std::filesystem::path& ply_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_ply_providers(providers); !result) {
        print_error(result.error().with_context("PLY exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.ply"); !result) {
        print_error(result.error().with_context("PLY exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_ply(document.value().meshes().begin()->second, ply_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << ply_path << " vertices=" << report.value().vertices
              << " faces=" << report.value().faces << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int export_stl(const std::filesystem::path& project_path, const std::filesystem::path& stl_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_stl_providers(providers); !result) {
        print_error(result.error().with_context("STL exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.stl"); !result) {
        print_error(result.error().with_context("STL exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_stl(document.value().meshes().begin()->second, stl_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << stl_path << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int import_mesh_to_project(
    const std::filesystem::path& source_path,
    const std::filesystem::path& project_path,
    carto::geometry::EditableMesh mesh,
    const carto::io::IoReport& report) {
    carto::application::ApplicationSession session;
    auto created_project = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{source_path.stem().string()});
    if (!created_project) {
        print_error(created_project.error());
        return EXIT_FAILURE;
    }
    auto baseline = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{project_path});
    if (!baseline) {
        print_error(baseline.error());
        return EXIT_FAILURE;
    }
    auto created_object = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{source_path.stem().string(), std::move(mesh)});
    if (!created_object) {
        print_error(created_object.error());
        return EXIT_FAILURE;
    }
    auto receipt = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{std::nullopt});
    if (!receipt) {
        print_error(receipt.error());
        return EXIT_FAILURE;
    }
    std::cout << "imported " << source_path << " -> " << project_path
              << " vertices=" << report.vertices << " faces=" << report.faces
              << " triangles=" << report.triangles << " journaled=true\n";
    for (const auto& warning : report.warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int import_obj(const std::filesystem::path& obj_path, const std::filesystem::path& project_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_obj_providers(providers); !result) {
        print_error(result.error().with_context("OBJ importer provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.import.obj"); !result) {
        print_error(result.error().with_context("OBJ importer capability"));
        return EXIT_FAILURE;
    }
    auto imported = carto::io::import_obj(obj_path);
    if (!imported) {
        print_error(imported.error());
        return EXIT_FAILURE;
    }
    return import_mesh_to_project(obj_path, project_path, std::move(imported.value().mesh),
                                  imported.value().report);
}

int import_ply(const std::filesystem::path& ply_path, const std::filesystem::path& project_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_ply_providers(providers); !result) {
        print_error(result.error().with_context("PLY importer provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.import.ply"); !result) {
        print_error(result.error().with_context("PLY importer capability"));
        return EXIT_FAILURE;
    }
    auto imported = carto::io::import_ply(ply_path);
    if (!imported) {
        print_error(imported.error());
        return EXIT_FAILURE;
    }
    return import_mesh_to_project(ply_path, project_path, std::move(imported.value().mesh),
                                  imported.value().report);
}

int import_stl(const std::filesystem::path& stl_path, const std::filesystem::path& project_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_stl_providers(providers); !result) {
        print_error(result.error().with_context("STL importer provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.import.stl"); !result) {
        print_error(result.error().with_context("STL importer capability"));
        return EXIT_FAILURE;
    }
    auto imported = carto::io::import_stl(stl_path);
    if (!imported) {
        print_error(imported.error());
        return EXIT_FAILURE;
    }
    return import_mesh_to_project(stl_path, project_path, std::move(imported.value().mesh),
                                  imported.value().report);
}

int export_gltf(const std::filesystem::path& project_path, const std::filesystem::path& gltf_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_gltf_providers(providers); !result) {
        print_error(result.error().with_context("glTF exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.gltf"); !result) {
        print_error(result.error().with_context("glTF exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_gltf(document.value(), gltf_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << gltf_path << " profile=cartographer-gltf-v1"
              << " objects=" << report.value().objects
              << " meshes=" << report.value().meshes
              << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles
              << " index_bytes=" << report.value().index_bytes
              << " index_type=" << (report.value().used_uint32_indices ? "uint32" : "uint16")
              << '\n';
    for (const auto& warning : report.value().warnings) {
        std::cout << "warning: " << warning << '\n';
    }
    return EXIT_SUCCESS;
}

void usage(std::ostream& output) {
    output << "Cartographer " << CARTOGRAPHER_VERSION << "\n"
           << "Usage:\n"
           << "  cartographer_cli demo <project.carto>\n"
           << "  cartographer_cli scientific-demo <project.carto>\n"
           << "  cartographer_cli scientific-reference-demo <project.carto>\n"
           << "  cartographer_cli validate <project.carto>\n"
           << "  cartographer_cli inspect-scientific <project.carto>\n"
           << "  cartographer_cli export-sequence-fasta <project.carto> <sequence.fasta>\n"
           << "  cartographer_cli import-sequence-fasta <sequence.fasta> <assembly> <sample-reference> <project.carto>\n"
           << "  cartographer_cli export-sequence-vcf <project.carto> <variants.vcf>\n"
           << "  cartographer_cli import-sequence-vcf <variants.vcf> <reference.carto> <project.carto>\n"
           << "  cartographer_cli export-sequence-gff3 <project.carto> <annotations.gff3>\n"
           << "  cartographer_cli import-sequence-gff3 <annotations.gff3> <reference.carto> <project.carto>\n"
           << "  cartographer_cli export-sequence-bed6 <project.carto> <annotations.bed>\n"
           << "  cartographer_cli import-sequence-bed6 <annotations.bed> <reference.carto> <project.carto>\n"
           << "  cartographer_cli export-sequence-gtf <project.carto> <annotations.gtf>\n"
           << "  cartographer_cli import-sequence-gtf <annotations.gtf> <reference.carto> <project.carto>\n"
           << "  cartographer_cli export-lineage-newick <project.carto> <lineage.newick>\n"
           << "  cartographer_cli import-lineage-newick <lineage.newick> <reference.carto> <project.carto>\n"
           << "  cartographer_cli export-anatomy-ontology-table <project.carto> <ontology.tsv>\n"
           << "  cartographer_cli import-anatomy-ontology-table <ontology.tsv> <reference.carto> <project.carto>\n"
           << "  cartographer_cli export-landmark-set <project.carto> <landmarks.carto-landmarks>\n"
           << "  cartographer_cli import-landmark-set <landmarks.carto-landmarks> <reference.carto> <project.carto>\n"
           << "  cartographer_cli inspect-functional-genome-exchange <exchange.carto-genome>\n"
           << "  cartographer_cli edit-functional-genome-exchange <exchange.carto-genome> <gene-id> <value> <edited.carto-genome>\n"
           << "  cartographer_cli export-functional-genome-exchange <project.carto> <exchange.carto-genome>\n"
           << "  cartographer_cli import-functional-genome-exchange <exchange.carto-genome> <project.carto>\n"
           << "  cartographer_cli inspect-anatomy-provider-candidate <candidate.carto-anatomy>\n"
           << "  cartographer_cli export-anatomy-provider-candidate <project.carto> <candidate.carto-anatomy>\n"
           << "  cartographer_cli export-obj <project.carto> <mesh.obj>\n"
           << "  cartographer_cli export-ply <project.carto> <mesh.ply>\n"
           << "  cartographer_cli export-stl <project.carto> <mesh.stl>\n"
           << "  cartographer_cli export-gltf <project.carto> <scene.gltf>\n"
           << "  cartographer_cli import-obj <mesh.obj> <project.carto>\n"
           << "  cartographer_cli import-ply <mesh.ply> <project.carto>\n"
           << "  cartographer_cli import-stl <mesh.stl> <project.carto>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(std::cerr);
        return EXIT_FAILURE;
    }
    const std::string_view command(argv[1]);
    if (command == "--help" || command == "help") {
        usage(std::cout);
        return EXIT_SUCCESS;
    }
    if (command == "--version") {
        std::cout << CARTOGRAPHER_VERSION << '\n';
        return EXIT_SUCCESS;
    }
    if (command == "demo" && argc == 3) {
        return demo(argv[2]);
    }
    if (command == "scientific-demo" && argc == 3) {
        return scientific_demo(argv[2]);
    }
    if (command == "scientific-reference-demo" && argc == 3) {
        return scientific_reference_demo(argv[2]);
    }
    if (command == "validate" && argc == 3) {
        return validate(argv[2]);
    }
    if (command == "inspect-scientific" && argc == 3) {
        return inspect_scientific(argv[2]);
    }
    if (command == "export-sequence-fasta" && argc == 4) {
        return export_sequence_fasta(argv[2], argv[3]);
    }
    if (command == "import-sequence-fasta" && argc == 6) {
        return import_sequence_fasta(argv[2], argv[3], argv[4], argv[5]);
    }
    if (command == "export-sequence-vcf" && argc == 4) {
        return export_sequence_vcf(argv[2], argv[3]);
    }
    if (command == "import-sequence-vcf" && argc == 5) {
        return import_sequence_vcf(argv[2], argv[3], argv[4]);
    }
    if (command == "export-sequence-gff3" && argc == 4) {
        return export_sequence_gff3(argv[2], argv[3]);
    }
    if (command == "import-sequence-gff3" && argc == 5) {
        return import_sequence_gff3(argv[2], argv[3], argv[4]);
    }
    if (command == "export-sequence-bed6" && argc == 4) {
        return export_sequence_bed6(argv[2], argv[3]);
    }
    if (command == "import-sequence-bed6" && argc == 5) {
        return import_sequence_bed6(argv[2], argv[3], argv[4]);
    }
    if (command == "export-sequence-gtf" && argc == 4) {
        return export_sequence_gtf(argv[2], argv[3]);
    }
    if (command == "import-sequence-gtf" && argc == 5) {
        return import_sequence_gtf(argv[2], argv[3], argv[4]);
    }
    if (command == "export-lineage-newick" && argc == 4) {
        return export_lineage_newick(argv[2], argv[3]);
    }
    if (command == "import-lineage-newick" && argc == 5) {
        return import_lineage_newick(argv[2], argv[3], argv[4]);
    }
    if (command == "export-anatomy-ontology-table" && argc == 4) {
        return export_anatomy_ontology_table(argv[2], argv[3]);
    }
    if (command == "import-anatomy-ontology-table" && argc == 5) {
        return import_anatomy_ontology_table(argv[2], argv[3], argv[4]);
    }
    if (command == "export-landmark-set" && argc == 4) {
        return export_landmark_set(argv[2], argv[3]);
    }
    if (command == "import-landmark-set" && argc == 5) {
        return import_landmark_set(argv[2], argv[3], argv[4]);
    }
    if (command == "inspect-functional-genome-exchange" && argc == 3) {
        return inspect_functional_genome_exchange(argv[2]);
    }
    if (command == "edit-functional-genome-exchange" && argc == 6) {
        return edit_functional_genome_exchange(argv[2], argv[3], argv[4], argv[5]);
    }
    if (command == "export-functional-genome-exchange" && argc == 4) {
        return export_functional_genome_exchange(argv[2], argv[3]);
    }
    if (command == "import-functional-genome-exchange" && argc == 4) {
        return import_functional_genome_exchange(argv[2], argv[3]);
    }
    if (command == "inspect-anatomy-provider-candidate" && argc == 3) {
        return inspect_anatomy_provider_candidate(argv[2]);
    }
    if (command == "export-anatomy-provider-candidate" && argc == 4) {
        return export_anatomy_provider_candidate(argv[2], argv[3]);
    }
    if (command == "export-obj" && argc == 4) {
        return export_obj(argv[2], argv[3]);
    }
    if (command == "export-ply" && argc == 4) {
        return export_ply(argv[2], argv[3]);
    }
    if (command == "export-stl" && argc == 4) {
        return export_stl(argv[2], argv[3]);
    }
    if (command == "export-gltf" && argc == 4) {
        return export_gltf(argv[2], argv[3]);
    }
    if (command == "import-obj" && argc == 4) {
        return import_obj(argv[2], argv[3]);
    }
    if (command == "import-ply" && argc == 4) {
        return import_ply(argv[2], argv[3]);
    }
    if (command == "import-stl" && argc == 4) {
        return import_stl(argv[2], argv[3]);
    }
    usage(std::cerr);
    return EXIT_FAILURE;
}
