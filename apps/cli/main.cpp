#include <carto/io/obj.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/application/application.hpp>
#if CARTO_CLI_HAS_D3D12_BAKE
#include <carto/d3d12/bake_graph.hpp>
#endif
#include <carto/assets/blob_store.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/production/cloth_solver.hpp>
#include <carto/production/preparation_operation.hpp>
#include <carto/production/preparation_scope.hpp>
#include <carto/providers/registry.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <chrono>
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

Result<std::uint64_t> parse_mesh_asset_id_argument(std::string_view text) {
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0U) {
        return Result<std::uint64_t>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_argument,
            "export mesh asset id must be a non-zero unsigned integer"));
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

Result<std::uint64_t> parse_bake_u64_argument(
    std::string_view text,
    std::string_view label) {
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return Result<std::uint64_t>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_argument,
            std::string(label) + " must be an unsigned integer"));
    }
    return Result<std::uint64_t>::success(value);
}

Result<std::uint32_t> parse_bake_u32_argument(
    std::string_view text,
    std::string_view label) {
    const auto value = parse_bake_u64_argument(text, label);
    if (!value || value.value() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::uint32_t>::failure(value
            ? carto::core::Diagnostic(carto::core::ErrorCode::invalid_argument,
                std::string(label) + " exceeds the supported unsigned integer range")
            : value.error());
    }
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(value.value()));
}

Result<double> parse_bake_positive_double_argument(
    std::string_view text,
    std::string_view label) {
    const std::string token(text);
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value) || value <= 0.0) {
        return Result<double>::failure(carto::core::Diagnostic(
            carto::core::ErrorCode::invalid_argument,
            std::string(label) + " must be finite and positive"));
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

int export_obj(
    const std::filesystem::path& project_path,
    const std::filesystem::path& obj_path,
    std::optional<std::uint64_t> mesh_asset) {
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
    const auto resolved_mesh = carto::production::resolve_single_mesh_export(
        document.value(), mesh_asset);
    if (!resolved_mesh) {
        print_error(resolved_mesh.error());
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_obj(
        document.value().meshes().at(resolved_mesh.value()), obj_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << obj_path << " mesh_asset=" << resolved_mesh.value()
              << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) {
        std::cout << "warning: " << warning << '\n';
    }
    return EXIT_SUCCESS;
}

int export_ply(
    const std::filesystem::path& project_path,
    const std::filesystem::path& ply_path,
    std::optional<std::uint64_t> mesh_asset) {
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
    const auto resolved_mesh = carto::production::resolve_single_mesh_export(
        document.value(), mesh_asset);
    if (!resolved_mesh) {
        print_error(resolved_mesh.error());
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_ply(
        document.value().meshes().at(resolved_mesh.value()), ply_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << ply_path << " mesh_asset=" << resolved_mesh.value()
              << " vertices=" << report.value().vertices
              << " faces=" << report.value().faces << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int export_stl(
    const std::filesystem::path& project_path,
    const std::filesystem::path& stl_path,
    std::optional<std::uint64_t> mesh_asset) {
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
    const auto resolved_mesh = carto::production::resolve_single_mesh_export(
        document.value(), mesh_asset);
    if (!resolved_mesh) {
        print_error(resolved_mesh.error());
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_stl(
        document.value().meshes().at(resolved_mesh.value()), stl_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << stl_path << " mesh_asset=" << resolved_mesh.value()
              << " vertices=" << report.value().vertices
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

const char* preparation_stage_name(carto::production::PreparationStage stage) {
    using carto::production::PreparationStage;
    switch (stage) {
    case PreparationStage::resolve: return "resolve";
    case PreparationStage::plan: return "plan";
    case PreparationStage::validate: return "validate";
    case PreparationStage::execute: return "execute";
    case PreparationStage::verify: return "verify";
    case PreparationStage::publish: return "publish";
    }
    return "unknown";
}

Result<carto::production::PreparationProfile> load_preparation_profile(
    const std::filesystem::path& path) {
    const auto text = read_bounded_text(
        path, carto::production::kMaxPreparationProfileBytes,
        "preparation profile");
    if (!text) {
        return Result<carto::production::PreparationProfile>::failure(
            text.error());
    }
    const auto profile = carto::production::PreparationProfile::deserialize(
        text.value());
    if (!profile) {
        return Result<carto::production::PreparationProfile>::failure(
            profile.error().with_context("preparation profile " + path.string()));
    }
    return profile;
}

int write_default_preparation_profile(const std::filesystem::path& path) {
    const auto profile = carto::production::portable_preparation_profile();
    const auto digest = profile.canonical_digest();
    if (!digest) {
        print_error(digest.error());
        return EXIT_FAILURE;
    }
    if (const auto written = write_new_text_atomically(
            path, profile.serialize(), "preparation profile"); !written) {
        print_error(written.error());
        return EXIT_FAILURE;
    }
    std::cout << "preparation_profile_written"
              << " identity=" << profile.identity
              << " digest=" << digest.value().hex()
              << " path=" << std::quoted(path.string()) << '\n';
    return EXIT_SUCCESS;
}

int inspect_preparation_profile(const std::filesystem::path& path) {
    const auto profile = load_preparation_profile(path);
    if (!profile) {
        print_error(profile.error());
        return EXIT_FAILURE;
    }
    const auto digest = profile.value().canonical_digest();
    if (!digest) {
        print_error(digest.error());
        return EXIT_FAILURE;
    }
    std::cout << "preparation_profile"
              << " identity=" << profile.value().identity
              << " digest=" << digest.value().hex()
              << " active_render_uv="
              << (profile.value().use_active_render_uv ? "true" : "false")
              << " unique_lightmap_uv="
              << (profile.value().include_unique_lightmap_uv ? "true" : "false")
              << " tangents="
              << (profile.value().generate_tangents_when_uv_present ? "true" : "false")
              << " lookdev="
              << (profile.value().include_lookdev ? "true" : "false")
              << " required_lookdev="
              << (profile.value().require_lookdev_for_material_regions ? "true" : "false")
              << '\n';
    return EXIT_SUCCESS;
}

int prepare_project(int argc, char** argv) {
    using namespace carto;
    const std::filesystem::path project_path(argv[2]);
    const std::filesystem::path output_root(argv[3]);
    int scope_index = 4;
    production::PreparationProfile profile =
        production::portable_preparation_profile();
    if (argc >= 7 && std::string_view(argv[scope_index]) == "--profile") {
        const auto loaded = load_preparation_profile(argv[scope_index + 1]);
        if (!loaded) {
            print_error(loaded.error());
            return EXIT_FAILURE;
        }
        profile = loaded.value();
        scope_index += 2;
    }
    if (scope_index >= argc) {
        std::cerr << "error: prepare requires an explicit scope\n";
        return EXIT_FAILURE;
    }
    const std::string_view scope_name(argv[scope_index]);
    const int scope_argument_count = argc - scope_index;
    auto document = project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }

    production::PreparationScopeRequest scope;
    scope.expected_source_revision = document.value().revision();
    if (scope_name == "scene" && scope_argument_count == 1) {
        scope.kind = production::PreparationScopeKind::scene;
    } else if (scope_name == "asset" && scope_argument_count == 2) {
        scope.kind = production::PreparationScopeKind::asset;
        const auto identity = parse_mesh_asset_id_argument(argv[scope_index + 1]);
        if (!identity) {
            print_error(identity.error());
            return EXIT_FAILURE;
        }
        scope.asset_ids.push_back(identity.value());
    } else if (scope_name == "assembly" && scope_argument_count == 2) {
        scope.kind = production::PreparationScopeKind::assembly;
        const auto identity = parse_mesh_asset_id_argument(argv[scope_index + 1]);
        if (!identity) {
            print_error(identity.error());
            return EXIT_FAILURE;
        }
        scope.node_ids.push_back(identity.value());
    } else if (scope_name == "selection" && scope_argument_count >= 2) {
        scope.kind = production::PreparationScopeKind::selection;
        for (int index = scope_index + 1; index < argc; ++index) {
            const auto identity = parse_mesh_asset_id_argument(argv[index]);
            if (!identity) {
                print_error(identity.error());
                return EXIT_FAILURE;
            }
            scope.node_ids.push_back(identity.value());
        }
        std::sort(scope.node_ids.begin(), scope.node_ids.end());
    } else {
        std::cerr << "error: prepare scope must be scene, asset <mesh-id>, "
                     "assembly <node-id>, or selection <node-id>...\n";
        return EXIT_FAILURE;
    }

    const auto current = production::read_selected_prepared_scene(output_root);
    if (!current) {
        print_error(current.error().with_context("prepared output baseline"));
        return EXIT_FAILURE;
    }
    std::optional<assets::Sha256Digest> expected;
    if (current.value().has_value()) {
        expected = current.value()->prepared_scene_digest;
    }
    auto source_root = project_path.parent_path();
    if (source_root.empty()) source_root = std::filesystem::current_path();
    const production::PreparationOperationRequest request{
        .scope = std::move(scope),
        .profile = std::move(profile),
        .source_root = std::move(source_root),
        .output_root = output_root,
        .expected_selected_digest = expected,
        .stop_token = {},
    };
    const auto report = production::prepare_and_publish(document.value(), request);
    for (const auto& diagnostic : report.diagnostics) {
        std::cerr << "diagnostic code=" << diagnostic.code
                  << " stage=" << preparation_stage_name(diagnostic.stage)
                  << " message=" << std::quoted(diagnostic.message);
        if (diagnostic.source_primary.has_value()) {
            std::cerr << " source_primary=" << diagnostic.source_primary.value();
        }
        if (diagnostic.source_secondary.has_value()) {
            std::cerr << " source_secondary=" << diagnostic.source_secondary.value();
        }
        if (!diagnostic.capability.empty()) {
            std::cerr << " capability=" << diagnostic.capability;
        }
        std::cerr << '\n';
    }
    if (!report.succeeded()) return EXIT_FAILURE;
    std::cout << "prepared"
              << " operation=" << report.operation_id.hex()
              << " source_revision=" << report.source_revision.value()
              << " profile=" << report.envelope->profile_identity
              << " profile_digest=" << report.envelope->profile_digest.hex()
              << " assets=" << report.resolved_scope->asset_ids.size()
              << " nodes=" << report.resolved_scope->node_ids.size()
              << " products=" << report.envelope->products.size()
              << " revision=" << report.publication->prepared_scene_digest.hex()
              << " reused="
              << (report.publication->reused_immutable_revision ? "true" : "false")
              << " output=" << std::quoted(output_root.string()) << '\n';
    return EXIT_SUCCESS;
}

int bake_ambient_occlusion(char** argv, bool use_d3d12_backend = false) {
    using namespace carto;
    const std::filesystem::path project_path(argv[2]);
    const auto mesh_asset = parse_mesh_asset_id_argument(argv[3]);
    const auto uv_set = parse_bake_u64_argument(argv[4], "UV set id");
    const auto width = parse_bake_u32_argument(argv[5], "texture width");
    const auto height = parse_bake_u32_argument(argv[6], "texture height");
    const auto tile_u = parse_bake_u32_argument(argv[7], "UDIM tile U");
    const auto tile_v = parse_bake_u32_argument(argv[8], "UDIM tile V");
    const auto sample_count = parse_bake_u32_argument(argv[9], "AO sample count");
    const auto max_distance = parse_bake_positive_double_argument(
        argv[10], "AO maximum distance");
    const auto seed = parse_bake_u64_argument(argv[11], "AO seed");
    const std::filesystem::path output_root(argv[12]);
    const auto report_parse_failure = [](const auto& result) {
        if (result) return false;
        print_error(result.error());
        return true;
    };
    if (report_parse_failure(mesh_asset) || report_parse_failure(uv_set) ||
        report_parse_failure(width) || report_parse_failure(height) ||
        report_parse_failure(tile_u) || report_parse_failure(tile_v) ||
        report_parse_failure(sample_count) || report_parse_failure(max_distance) ||
        report_parse_failure(seed)) return EXIT_FAILURE;
    if (output_root.empty() || output_root.filename().empty()) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_argument,
            "bake output directory must name a new directory"));
        return EXIT_FAILURE;
    }

    const auto document = project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto mesh = document.value().meshes().find(mesh_asset.value());
    if (mesh == document.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "AO bake mesh asset does not exist in the project"));
        return EXIT_FAILURE;
    }
    const std::string mesh_identity = document.value().source_namespace() +
        "/mesh/" + std::to_string(mesh_asset.value());
    const auto source_binding = production::bind_bake_source(mesh_identity, mesh->second);
    const auto target_binding = production::bind_bake_target(
        mesh_identity, mesh->second, uv_set.value());
    if (!source_binding || !target_binding) {
        print_error(!source_binding ? source_binding.error() : target_binding.error());
        return EXIT_FAILURE;
    }

    production::AmbientOcclusionRecipe recipe;
    recipe.binding.identity = "bake/ao/mesh/" + std::to_string(mesh_asset.value()) +
        "/uv/" + std::to_string(uv_set.value()) + "/tile/" +
        std::to_string(tile_u.value()) + "/" + std::to_string(tile_v.value());
    recipe.binding.source = source_binding.value();
    recipe.binding.target = target_binding.value();
    recipe.binding.target_uv_set = uv_set.value();
    recipe.binding.width = width.value();
    recipe.binding.height = height.value();
    recipe.binding.tile_u = tile_u.value();
    recipe.binding.tile_v = tile_v.value();
    recipe.sample_count = sample_count.value();
    recipe.max_distance = max_distance.value();
    recipe.origin_bias = 1e-4;
    recipe.seed = seed.value();
    if (auto result = recipe.validate(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) {
        print_error(recipe_digest.error());
        return EXIT_FAILURE;
    }

    std::optional<production::AmbientOcclusionExecutionReport> cpu_report;
#if CARTO_CLI_HAS_D3D12_BAKE
    std::optional<d3d12::D3D12AmbientOcclusionBakeReport> d3d12_report;
#endif
    const production::AmbientOcclusionMapArtifact* artifact = nullptr;
    std::string platform_profile = "carto.cpu-reference.v1";
    const auto started = std::chrono::steady_clock::now();
    if (use_d3d12_backend) {
#if CARTO_CLI_HAS_D3D12_BAKE
        const auto device_result = d3d12::D3D12Device::create();
        if (!device_result) {
            print_error(device_result.error().with_context("D3D12 AO bake worker"));
            return EXIT_FAILURE;
        }
        d3d12_report.emplace(d3d12::compile_ambient_occlusion_d3d12(
            *device_result.value(), recipe, mesh->second, mesh->second));
        for (const auto& diagnostic : d3d12_report->bake.diagnostics) {
            std::cerr << "diagnostic code=" << diagnostic.code
                      << " message=" << std::quoted(diagnostic.message) << '\n';
        }
        if (!d3d12_report->succeeded()) return EXIT_FAILURE;
        const auto profile = d3d12::d3d12_bake_platform_profile(
            *d3d12_report->projection);
        if (!profile) {
            print_error(profile.error());
            return EXIT_FAILURE;
        }
        platform_profile = profile.value();
        artifact = &*d3d12_report->bake.artifact;
#else
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "D3D12 AO backend is not enabled in this CLI build"));
        return EXIT_FAILURE;
#endif
    } else {
        cpu_report.emplace(production::compile_ambient_occlusion_cpu(
            recipe, mesh->second, mesh->second));
        for (const auto& diagnostic : cpu_report->diagnostics) {
            std::cerr << "diagnostic code=" << diagnostic.code
                      << " message=" << std::quoted(diagnostic.message) << '\n';
        }
        if (!cpu_report->succeeded() || !cpu_report->artifact.has_value()) {
            return EXIT_FAILURE;
        }
        artifact = &*cpu_report->artifact;
    }
    const auto finished = std::chrono::steady_clock::now();

    const auto current_document = project::ProjectDocument::load(project_path);
    if (!current_document) {
        print_error(current_document.error().with_context(
            "reloading project before AO bake publication"));
        return EXIT_FAILURE;
    }
    if (current_document.value().source_namespace() !=
        document.value().source_namespace()) {
        print_error(core::Diagnostic(core::ErrorCode::stale_data,
            "project source namespace changed while the AO bake was running"));
        return EXIT_FAILURE;
    }
    const auto current_mesh = current_document.value().meshes().find(mesh_asset.value());
    if (current_mesh == current_document.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::stale_data,
            "project AO bake mesh disappeared while the bake was running"));
        return EXIT_FAILURE;
    }
    if (auto result = production::validate_bake_sources(
            recipe.binding, current_mesh->second, current_mesh->second); !result) {
        print_error(result.error().with_context(
            "revalidating project mesh before AO bake publication"));
        return EXIT_FAILURE;
    }

    const auto payload = artifact->serialize();
    const auto cook_key = production::bake_ambient_occlusion_cook_key(
        recipe, platform_profile);
    if (!payload || !cook_key) {
        print_error(!payload ? payload.error() : cook_key.error());
        return EXIT_FAILURE;
    }
    production::ProductionArtifactReceipt receipt;
    receipt.key = cook_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = payload.value().size();
    if (use_d3d12_backend) {
        receipt.quality.measured_error = 0.0;
        receipt.quality.importance = 1.0;
        receipt.quality.confidence = 1.0;
    }
    receipt.cost.cook_milliseconds = std::chrono::duration<double, std::milli>(
        finished - started).count();
    receipt.cost.disk_bytes = receipt.output_bytes;
    if (auto result = receipt.refresh_digest(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }

    std::error_code output_error;
    const auto parent = output_root.parent_path();
    if (!parent.empty() &&
        (!std::filesystem::exists(parent, output_error) || output_error ||
         !std::filesystem::is_directory(parent, output_error) || output_error)) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            "bake output parent directory is unavailable"));
        return EXIT_FAILURE;
    }
    output_error.clear();
    if (std::filesystem::exists(output_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "bake output directory refuses to overwrite an existing path"));
        return EXIT_FAILURE;
    }
    std::filesystem::path staging_root = output_root;
    staging_root += ".pending-" + recipe_digest.value().hex().substr(0U, 16U);
    output_error.clear();
    if (std::filesystem::exists(staging_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "bake staging directory already exists and requires inspection"));
        return EXIT_FAILURE;
    }
    output_error.clear();
    if (!std::filesystem::create_directory(staging_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new bake staging directory"));
        return EXIT_FAILURE;
    }

    try {
        assets::BlobStore blobs(staging_root / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        const production::AuthoritativeSource current_source{
            recipe.binding.source.identity,
            recipe.binding.source.revision,
            recipe.binding.source.content_digest};
        const production::AdmissionPolicy policy;
        if (use_d3d12_backend) {
#if CARTO_CLI_HAS_D3D12_BAKE
            if (auto result = d3d12::publish_ambient_occlusion_map_d3d12(
                    artifact_store, receipt, *d3d12_report,
                    current_mesh->second, current_mesh->second, policy,
                    platform_profile); !result) {
                print_error(result.error());
                return EXIT_FAILURE;
            }
#endif
        } else if (auto result = production::publish_ambient_occlusion_map(
                artifact_store, receipt, *artifact, current_mesh->second,
                current_mesh->second, policy, platform_profile); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        const auto reopened = production::read_ambient_occlusion_map(
            artifact_store, recipe, current_mesh->second, current_mesh->second,
            policy, platform_profile);
        if (!reopened) {
            print_error(reopened.error());
            return EXIT_FAILURE;
        }
        const std::string ledger_text = artifact_store.ledger().serialize();
        const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_text);
        if (!ledger || ledger.value().size() != 1U) {
            print_error(ledger ? core::Diagnostic(core::ErrorCode::validation_failed,
                "AO bake receipt ledger did not reopen with exactly one record")
                : ledger.error());
            return EXIT_FAILURE;
        }
        const auto manifest_write = write_new_text_atomically(
            staging_root / "receipts.carto-production-receipts", ledger_text,
            "AO bake receipt ledger");
        if (!manifest_write) {
            print_error(manifest_write.error());
            return EXIT_FAILURE;
        }
        const auto reopened_manifest = read_bounded_text(
            staging_root / "receipts.carto-production-receipts",
            production::ProductionReceiptLedger::max_serialized_bytes,
            "AO bake receipt ledger");
        if (!reopened_manifest ||
            !production::ProductionReceiptLedger::deserialize(reopened_manifest.value())) {
            print_error(reopened_manifest
                ? core::Diagnostic(core::ErrorCode::validation_failed,
                    "AO bake receipt manifest failed read-back validation")
                : reopened_manifest.error());
            return EXIT_FAILURE;
        }
        if (reopened.value().artifact_digest != artifact->artifact_digest) {
            print_error(core::Diagnostic(core::ErrorCode::validation_failed,
                "published AO artifact does not match the compiled artifact"));
            return EXIT_FAILURE;
        }
        output_error.clear();
        std::filesystem::rename(staging_root, output_root, output_error);
        if (output_error) {
            print_error(core::Diagnostic(core::ErrorCode::io_error,
                "atomic bake output publication failed; inspect the staging directory"));
            return EXIT_FAILURE;
        }
        std::cout << "baked"
                  << " kind=ambient-occlusion"
                  << " evidence=" << (use_d3d12_backend
                      ? "carto.d3d12.cpu-reference-parity" : "carto.cpu-reference")
                  << " platform_profile=" << std::quoted(platform_profile)
                  << " source=" << std::quoted(current_source.identity)
                  << " source_revision=" << current_source.revision.value()
                  << " source_digest=" << current_source.content_digest.hex()
                  << " mesh=" << mesh_asset.value()
                  << " uv_set=" << uv_set.value()
                  << " tile=" << tile_u.value() << ',' << tile_v.value()
                  << " covered_texels=" << artifact->covered_texels
                  << " rays_cast=" << artifact->rays_cast
                  << " sampling_standard_error_upper_bound="
                  << artifact->sampling_standard_error_upper_bound
                  << " artifact_digest=" << artifact->artifact_digest.hex()
                  << " payload_digest=" << receipt.output_digest.hex()
                  << " receipt_digest=" << receipt.receipt_digest.hex();
#if CARTO_CLI_HAS_D3D12_BAKE
        if (d3d12_report.has_value()) {
            const auto& worker = *d3d12_report->projection;
            std::cout << " adapter=" << std::quoted(worker.adapter_name)
                      << " adapter_id=" << std::quoted(worker.stable_adapter_id)
                      << " driver=" << std::quoted(worker.driver)
                      << " request_digest=" << worker.request_digest.hex()
                      << " shader_source_digest=" << worker.shader_source_digest.hex()
                      << " shader_binary_digest=" << worker.shader_binary_digest.hex()
                      << " worker_wall_nanoseconds=" << worker.wall_time_nanoseconds;
        }
#endif
        std::cout << " output=" << std::quoted(output_root.string()) << '\n';
    } catch (const std::exception& error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            std::string("AO bake publication failed: ") + error.what()));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int bake_cloth_rest_pose(char** argv) {
    using namespace carto;
    const std::filesystem::path project_path(argv[2]);
    const auto mesh_asset = parse_mesh_asset_id_argument(argv[3]);
    const auto frame_count = parse_bake_u32_argument(argv[4], "cloth frame count");
    const auto substeps = parse_bake_u32_argument(argv[5], "cloth substeps per frame");
    const auto iterations = parse_bake_u32_argument(argv[6], "cloth projection iterations");
    const auto settle_window = parse_bake_u32_argument(argv[7], "cloth settle window");
    const auto settle_tolerance = parse_bake_positive_double_argument(
        argv[8], "cloth settle displacement tolerance");
    const std::filesystem::path output_root(argv[9]);
    const auto report_parse_failure = [](const auto& result) {
        if (result) return false;
        print_error(result.error());
        return true;
    };
    if (report_parse_failure(mesh_asset) || report_parse_failure(frame_count) ||
        report_parse_failure(substeps) || report_parse_failure(iterations) ||
        report_parse_failure(settle_window) || report_parse_failure(settle_tolerance)) {
        return EXIT_FAILURE;
    }
    if (output_root.empty() || output_root.filename().empty()) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_argument,
            "cloth bake output directory must name a new directory"));
        return EXIT_FAILURE;
    }

    auto loaded_project = project::ProjectDocument::load(project_path);
    if (!loaded_project) {
        print_error(loaded_project.error());
        return EXIT_FAILURE;
    }
    project::ProjectDocument document = std::move(loaded_project).value();
    const core::Revision input_project_revision = document.revision();
    const auto mesh = document.meshes().find(mesh_asset.value());
    const auto garment_record = document.garment_assets().find(mesh_asset.value());
    if (mesh == document.meshes().end() || garment_record == document.garment_assets().end()) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "cloth bake requires an existing mesh with an authored GarmentAsset"));
        return EXIT_FAILURE;
    }
    const project::SerializedGarmentAsset source_garment_record = garment_record->second;
    auto garment = production::GarmentAsset::deserialize(garment_record->second.bytes);
    if (!garment) {
        print_error(garment.error().with_context("project GarmentAsset"));
        return EXIT_FAILURE;
    }
    if (garment.value().mesh_asset != mesh_asset.value()) {
        print_error(core::Diagnostic(core::ErrorCode::stale_data,
            "GarmentAsset mesh identity differs from the requested source mesh"));
        return EXIT_FAILURE;
    }
    if (auto result = garment.value().validate(mesh->second); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }

    production::ClothSolverSettings settings;
    settings.frame_count = frame_count.value();
    settings.substeps_per_frame = substeps.value();
    settings.projection_iterations = iterations.value();
    settings.settle_window_frames = settle_window.value();
    settings.settle_displacement_tolerance = settle_tolerance.value();
    settings.require_settled = true;
    if (auto result = settings.validate(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }

    const auto cook_started = std::chrono::steady_clock::now();
    const auto simulation = production::compile_cloth_cpu(
        garment.value(), mesh->second, settings);
    if (!simulation) {
        print_error(simulation.error().with_context("CPU cloth simulation"));
        return EXIT_FAILURE;
    }
    const auto baked = production::bake_cloth_settled_rest_pose(
        simulation.value(), garment.value(), mesh->second);
    if (!baked) {
        print_error(baked.error().with_context("settled cloth rest-pose bake"));
        return EXIT_FAILURE;
    }
    const auto simulation_payload = simulation.value().serialize();
    const auto simulation_key = production::cloth_production_cook_key(
        garment.value(), settings, "cpu-reference");
    const auto rest_pose_key = production::cloth_rest_pose_bake_cook_key(
        simulation.value(), "cpu-reference");
    const auto rest_pose_payload = baked.value().receipt.serialize();
    if (!simulation_payload || !simulation_key || !rest_pose_key || !rest_pose_payload) {
        const core::Diagnostic& failure = !simulation_payload ? simulation_payload.error() :
            (!simulation_key ? simulation_key.error() :
                (!rest_pose_key ? rest_pose_key.error() : rest_pose_payload.error()));
        print_error(failure);
        return EXIT_FAILURE;
    }
    const auto ensure_input_unchanged = [&]() -> core::Result<void> {
        const auto latest = project::ProjectDocument::load(project_path);
        if (!latest) return core::Result<void>::failure(latest.error());
        const auto latest_mesh = latest.value().meshes().find(mesh_asset.value());
        const auto latest_garment = latest.value().garment_assets().find(mesh_asset.value());
        if (latest.value().revision() != input_project_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source project revision changed while the cloth bake was compiling"));
        }
        if (latest.value().source_namespace() != document.source_namespace()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source project namespace changed while the cloth bake was compiling"));
        }
        if (latest_mesh == latest.value().meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source mesh disappeared while the cloth bake was compiling"));
        }
        if (latest_garment == latest.value().garment_assets().end() ||
            latest_garment->second != source_garment_record) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "authored GarmentAsset changed while the cloth bake was compiling"));
        }
        const auto latest_binding = production::bind_bake_source(
            simulation.value().source.identity, latest_mesh->second);
        if (!latest_binding || latest_binding.value() != simulation.value().source) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source mesh changed while the cloth bake was compiling"));
        }
        return core::Result<void>::success();
    };
    if (auto result = ensure_input_unchanged(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto cook_finished = std::chrono::steady_clock::now();
    const double cook_milliseconds = std::chrono::duration<double, std::milli>(
        cook_finished - cook_started).count();

    production::ProductionArtifactReceipt simulation_receipt;
    simulation_receipt.key = simulation_key.value();
    simulation_receipt.status = production::ReceiptStatus::admitted;
    simulation_receipt.output_digest = assets::sha256(simulation_payload.value());
    simulation_receipt.output_bytes = simulation_payload.value().size();
    simulation_receipt.quality.measured_error = simulation.value().maximum_constraint_error;
    simulation_receipt.quality.importance = 1.0;
    simulation_receipt.quality.confidence = 1.0;
    simulation_receipt.cost.cpu_milliseconds = cook_milliseconds;
    simulation_receipt.cost.disk_bytes = simulation_receipt.output_bytes;
    if (auto result = simulation_receipt.refresh_digest(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    production::ProductionArtifactReceipt rest_pose_receipt;
    rest_pose_receipt.key = rest_pose_key.value();
    rest_pose_receipt.status = production::ReceiptStatus::admitted;
    rest_pose_receipt.output_digest = assets::sha256(rest_pose_payload.value());
    rest_pose_receipt.output_bytes = rest_pose_payload.value().size();
    rest_pose_receipt.quality.measured_error = simulation.value().maximum_constraint_error;
    rest_pose_receipt.quality.importance = 1.0;
    rest_pose_receipt.quality.confidence = 1.0;
    rest_pose_receipt.cost.cpu_milliseconds = cook_milliseconds;
    rest_pose_receipt.cost.disk_bytes = rest_pose_receipt.output_bytes;
    if (auto result = rest_pose_receipt.refresh_digest(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }

    std::error_code output_error;
    const auto parent = output_root.parent_path();
    if (!parent.empty() &&
        (!std::filesystem::exists(parent, output_error) || output_error ||
         !std::filesystem::is_directory(parent, output_error) || output_error)) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            "cloth bake output parent directory is unavailable"));
        return EXIT_FAILURE;
    }
    output_error.clear();
    if (std::filesystem::exists(output_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "cloth bake output directory refuses to overwrite an existing path"));
        return EXIT_FAILURE;
    }
    std::filesystem::path staging_root = output_root;
    staging_root += ".pending-" + simulation.value().artifact_digest.hex().substr(0U, 16U);
    output_error.clear();
    if (std::filesystem::exists(staging_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "cloth bake staging directory already exists and requires inspection"));
        return EXIT_FAILURE;
    }
    output_error.clear();
    if (!std::filesystem::create_directory(staging_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new cloth bake staging directory"));
        return EXIT_FAILURE;
    }

    try {
      {
        assets::BlobStore blobs(staging_root / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        production::AdmissionPolicy simulation_policy;
        simulation_policy.max_output_bytes = production::kMaxClothSimulationArtifactBytes;
        production::AdmissionPolicy rest_pose_policy;
        rest_pose_policy.max_output_bytes = production::kMaxClothRestPoseReceiptBytes;
        if (auto result = production::publish_cloth_simulation(
                artifact_store, simulation_receipt, simulation.value(), garment.value(),
                mesh->second, simulation_policy); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        if (auto result = production::publish_cloth_rest_pose_bake(
                artifact_store, rest_pose_receipt, baked.value(), simulation.value(),
                garment.value(), mesh->second, rest_pose_policy); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        const auto reopened_simulation = production::read_cloth_simulation(
            artifact_store, garment.value(), mesh->second, settings,
            "cpu-reference", simulation_policy);
        const auto reopened_bake = production::read_cloth_rest_pose_bake(
            artifact_store, simulation.value(), garment.value(), mesh->second,
            "cpu-reference", rest_pose_policy);
        if (!reopened_simulation || !reopened_bake ||
            reopened_simulation.value().artifact_digest != simulation.value().artifact_digest ||
            reopened_bake.value().receipt.canonical() != baked.value().receipt.canonical()) {
            print_error(!reopened_simulation ? reopened_simulation.error() :
                (!reopened_bake ? reopened_bake.error() : core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "published cloth artifacts differ from their deterministic compile")));
            return EXIT_FAILURE;
        }
        const std::string ledger_text = artifact_store.ledger().serialize();
        const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_text);
        if (!ledger || ledger.value().size() != 2U) {
            print_error(ledger ? core::Diagnostic(core::ErrorCode::validation_failed,
                "cloth bake receipt ledger did not reopen with exactly two records")
                : ledger.error());
            return EXIT_FAILURE;
        }
        const auto ledger_write = write_new_text_atomically(
            staging_root / "receipts.carto-production-receipts", ledger_text,
            "cloth bake receipt ledger");
        if (!ledger_write) {
            print_error(ledger_write.error());
            return EXIT_FAILURE;
        }
        const auto reopened_ledger_text = read_bounded_text(
            staging_root / "receipts.carto-production-receipts",
            production::ProductionReceiptLedger::max_serialized_bytes,
            "cloth bake receipt ledger");
        if (!reopened_ledger_text ||
            !production::ProductionReceiptLedger::deserialize(reopened_ledger_text.value())) {
            print_error(reopened_ledger_text
                ? core::Diagnostic(core::ErrorCode::validation_failed,
                    "cloth bake receipt ledger failed read-back validation")
                : reopened_ledger_text.error());
            return EXIT_FAILURE;
        }
      }

      std::uint64_t output_mesh_asset_id = 0U;
      {
        journal::Journal project_journal(staging_root / "project.journal.log");
        const std::string input_snapshot = document.serialize();
        const auto* input_snapshot_bytes = reinterpret_cast<const std::uint8_t*>(
            input_snapshot.data());
        const auto anchored_snapshot = project_journal.append(journal::JournalAppend{
            core::Revision{}, document.revision(), "project.snapshot",
            std::span<const std::uint8_t>(input_snapshot_bytes, input_snapshot.size()),
            std::nullopt});
        if (!anchored_snapshot) {
            print_error(anchored_snapshot.error());
            return EXIT_FAILURE;
        }
        auto transaction = project::ProjectTransaction::begin(
            document, project_journal, "cartographer_cli");
        if (!transaction) {
            print_error(transaction.error());
            return EXIT_FAILURE;
        }
        const auto output_mesh_asset = transaction.value().add_mesh(baked.value().mesh);
        if (!output_mesh_asset) {
            print_error(output_mesh_asset.error());
            return EXIT_FAILURE;
        }
        output_mesh_asset_id = output_mesh_asset.value();
        if (auto result = production::stage_cloth_rest_pose_bake_receipt(
                transaction.value(), output_mesh_asset.value(), baked.value(),
                simulation.value(), garment.value(), mesh->second); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        const auto committed = transaction.value().commit("cloth.rest-pose.bake");
        if (!committed) {
            print_error(committed.error());
            return EXIT_FAILURE;
        }
        const auto project_write = document.save_atomic(staging_root / "project.carto");
        if (!project_write) {
            print_error(project_write.error());
            return EXIT_FAILURE;
        }
      }
        const auto reopened_project = project::ProjectDocument::load(
            staging_root / "project.carto");
        if (!reopened_project) {
            print_error(reopened_project.error());
            return EXIT_FAILURE;
        }
        const auto reopened_mesh = reopened_project.value().meshes().find(
            output_mesh_asset_id);
        const auto project_receipt = reopened_project.value().cloth_rest_pose_receipts().find(
            output_mesh_asset_id);
        if (reopened_mesh == reopened_project.value().meshes().end() ||
            project_receipt == reopened_project.value().cloth_rest_pose_receipts().end()) {
            print_error(core::Diagnostic(core::ErrorCode::validation_failed,
                "cloth bake project failed to reopen its derived mesh or receipt"));
            return EXIT_FAILURE;
        }
        const auto output_binding = production::bind_bake_source(
            baked.value().receipt.output.identity, reopened_mesh->second);
        const std::vector<std::uint8_t> project_receipt_bytes(
            project_receipt->second.bytes.begin(), project_receipt->second.bytes.end());
        const auto decoded_project_receipt = production::ClothRestPoseBakeReceipt::deserialize(
            std::span<const std::uint8_t>{project_receipt_bytes});
        if (!output_binding || output_binding.value() != baked.value().receipt.output ||
            !decoded_project_receipt || decoded_project_receipt.value().canonical() !=
                baked.value().receipt.canonical()) {
            print_error(core::Diagnostic(core::ErrorCode::validation_failed,
                "reopened project cloth mesh does not match its settled-pose receipt"));
            return EXIT_FAILURE;
        }

        if (auto result = ensure_input_unchanged(); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }

        output_error.clear();
        std::filesystem::rename(staging_root, output_root, output_error);
        if (output_error) {
            print_error(core::Diagnostic(core::ErrorCode::io_error,
                "atomic cloth bake publication failed; inspect the staging directory"));
            return EXIT_FAILURE;
        }
        std::cout << "baked"
                  << " kind=cloth-rest-pose"
                  << " evidence=" << simulation.value().evidence_class
                  << " source=" << std::quoted(simulation.value().source.identity)
                  << " source_revision=" << simulation.value().source.revision.value()
                  << " source_digest=" << simulation.value().source.content_digest.hex()
                  << " mesh=" << mesh_asset.value()
                  << " output_mesh=" << output_mesh_asset_id
                  << " settled_frame=" << simulation.value().settled_frame.value()
                  << " simulation_digest=" << simulation.value().artifact_digest.hex()
                  << " output_digest=" << baked.value().receipt.output.content_digest.hex()
                  << " receipt_digest=" << baked.value().receipt.receipt_digest.hex()
                  << " output=" << std::quoted(output_root.string()) << '\n';
    } catch (const std::exception& error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            std::string("cloth bake publication failed: ") + error.what()));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int inspect_cloth_rest_pose(const std::filesystem::path& output_root) {
    using namespace carto;
    const auto project = project::ProjectDocument::load(output_root / "project.carto");
    if (!project) {
        print_error(project.error());
        return EXIT_FAILURE;
    }
    if (project.value().cloth_rest_pose_receipts().size() != 1U) {
        print_error(core::Diagnostic(core::ErrorCode::validation_failed,
            "cloth package must contain exactly one project-bound rest-pose receipt"));
        return EXIT_FAILURE;
    }
    const auto& [output_mesh_asset, stored_receipt] =
        *project.value().cloth_rest_pose_receipts().begin();
    const std::vector<std::uint8_t> stored_receipt_bytes(
        stored_receipt.bytes.begin(), stored_receipt.bytes.end());
    if (assets::sha256(std::span<const std::uint8_t>{stored_receipt_bytes}) !=
        stored_receipt.digest) {
        print_error(core::Diagnostic(core::ErrorCode::validation_failed,
            "project cloth receipt digest does not bind its bytes"));
        return EXIT_FAILURE;
    }
    const auto project_receipt = production::ClothRestPoseBakeReceipt::deserialize(
        std::span<const std::uint8_t>{stored_receipt_bytes});
    if (!project_receipt) {
        print_error(project_receipt.error());
        return EXIT_FAILURE;
    }
    const auto output_mesh = project.value().meshes().find(output_mesh_asset);
    if (output_mesh == project.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "project cloth receipt points to a missing derived mesh"));
        return EXIT_FAILURE;
    }

    std::optional<production::GarmentAsset> source_garment;
    const geometry::EditableMesh* source_mesh = nullptr;
    for (const auto& [mesh_asset, record] : project.value().garment_assets()) {
        const auto candidate = production::GarmentAsset::deserialize(record.bytes);
        if (!candidate) {
            print_error(candidate.error().with_context("project GarmentAsset"));
            return EXIT_FAILURE;
        }
        if (candidate.value().project_namespace != project.value().source_namespace()) {
            continue;
        }
        if (candidate.value().source.identity != project_receipt.value().source.identity ||
            candidate.value().source.revision != project_receipt.value().source.revision ||
            candidate.value().source.content_digest !=
                project_receipt.value().source.content_digest) {
            continue;
        }
        const auto candidate_mesh = project.value().meshes().find(mesh_asset);
        if (candidate_mesh == project.value().meshes().end() ||
            !candidate.value().validate(candidate_mesh->second)) {
            print_error(core::Diagnostic(core::ErrorCode::stale_data,
                "project GarmentAsset no longer binds its source mesh"));
            return EXIT_FAILURE;
        }
        if (source_garment.has_value()) {
            print_error(core::Diagnostic(core::ErrorCode::validation_failed,
                "cloth receipt source binding matches multiple GarmentAssets"));
            return EXIT_FAILURE;
        }
        source_garment = candidate.value();
        source_mesh = &candidate_mesh->second;
    }
    if (!source_garment.has_value() || source_mesh == nullptr) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "cloth receipt source and GarmentAsset are missing from the package project"));
        return EXIT_FAILURE;
    }
    const auto garment_digest = source_garment->digest();
    if (!garment_digest || garment_digest.value() != project_receipt.value().garment_digest) {
        print_error(garment_digest ? core::Diagnostic(core::ErrorCode::stale_data,
            "cloth receipt garment digest differs from the project source")
            : garment_digest.error());
        return EXIT_FAILURE;
    }

    const auto ledger_text = read_bounded_text(
        output_root / "receipts.carto-production-receipts",
        production::ProductionReceiptLedger::max_serialized_bytes,
        "cloth bake production receipt ledger");
    if (!ledger_text) {
        print_error(ledger_text.error());
        return EXIT_FAILURE;
    }
    const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_text.value());
    if (!ledger || ledger.value().size() != 2U) {
        print_error(ledger ? core::Diagnostic(core::ErrorCode::validation_failed,
            "cloth package receipt ledger must contain exactly two records")
            : ledger.error());
        return EXIT_FAILURE;
    }
    const production::ProductionArtifactReceipt* simulation_record = nullptr;
    const production::ProductionArtifactReceipt* rest_pose_record = nullptr;
    for (const auto& record : ledger.value().records()) {
        if (record.status != production::ReceiptStatus::admitted) continue;
        if (record.key.kind == production::ArtifactKind::cloth_simulation) {
            simulation_record = &record;
        } else if (record.key.kind == production::ArtifactKind::cloth_rest_pose) {
            rest_pose_record = &record;
        }
    }
    if (simulation_record == nullptr || rest_pose_record == nullptr) {
        print_error(core::Diagnostic(core::ErrorCode::validation_failed,
            "cloth package is missing its admitted simulation or rest-pose receipt"));
        return EXIT_FAILURE;
    }

    production::AdmissionPolicy simulation_policy;
    simulation_policy.max_output_bytes = production::kMaxClothSimulationArtifactBytes;
    production::AdmissionPolicy rest_pose_policy;
    rest_pose_policy.max_output_bytes = production::kMaxClothRestPoseReceiptBytes;
    const production::AuthoritativeSource authoritative_source{
        project_receipt.value().source.identity,
        project_receipt.value().source.revision,
        project_receipt.value().source.content_digest};
    if (auto result = production::validate_publication(
            *simulation_record, authoritative_source, simulation_policy); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    if (auto result = production::validate_publication(
            *rest_pose_record, authoritative_source, rest_pose_policy); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    assets::BlobStore blobs(output_root / "blobs");
    const auto simulation_bytes = blobs.read(simulation_record->output_digest);
    const auto rest_pose_bytes = blobs.read(rest_pose_record->output_digest);
    if (!simulation_bytes || !rest_pose_bytes) {
        print_error(!simulation_bytes ? simulation_bytes.error() : rest_pose_bytes.error());
        return EXIT_FAILURE;
    }
    const auto simulation = production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{simulation_bytes.value()});
    if (!simulation) {
        print_error(simulation.error());
        return EXIT_FAILURE;
    }
    if (auto result = simulation.value().validate(*source_garment, *source_mesh); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto expected_simulation_key = production::cloth_production_cook_key(
        *source_garment, simulation.value().settings,
        simulation_record->key.platform_profile);
    const auto expected_rest_pose_key = production::cloth_rest_pose_bake_cook_key(
        simulation.value(), rest_pose_record->key.platform_profile);
    const auto stored_rest_pose = production::ClothRestPoseBakeReceipt::deserialize(
        std::span<const std::uint8_t>{rest_pose_bytes.value()});
    if (!expected_simulation_key || !expected_rest_pose_key || !stored_rest_pose ||
        expected_simulation_key.value() != simulation_record->key ||
        expected_rest_pose_key.value() != rest_pose_record->key ||
        stored_rest_pose.value().canonical() != project_receipt.value().canonical() ||
        stored_rest_pose.value().simulation_artifact_digest != simulation.value().artifact_digest) {
        print_error(core::Diagnostic(core::ErrorCode::stale_data,
            "cloth package artifacts do not match their source-bound cook receipts"));
        return EXIT_FAILURE;
    }
    const std::string stored_payload(
        rest_pose_bytes.value().begin(), rest_pose_bytes.value().end());
    if (stored_payload != stored_receipt.bytes) {
        print_error(core::Diagnostic(core::ErrorCode::validation_failed,
            "project and content-addressed cloth receipts disagree"));
        return EXIT_FAILURE;
    }
    const auto rebuilt = production::bake_cloth_settled_rest_pose(
        simulation.value(), *source_garment, *source_mesh);
    const auto output_binding = production::bind_bake_source(
        project_receipt.value().output.identity, output_mesh->second);
    if (!rebuilt || !output_binding ||
        rebuilt.value().receipt.canonical() != project_receipt.value().canonical() ||
        output_binding.value() != project_receipt.value().output) {
        print_error(!rebuilt ? rebuilt.error() : core::Diagnostic(
            core::ErrorCode::validation_failed,
            "cloth package derived mesh differs from its reconstructed receipt"));
        return EXIT_FAILURE;
    }

    std::cout << "verified"
              << " kind=cloth-rest-pose"
              << " source=" << std::quoted(project_receipt.value().source.identity)
              << " source_revision=" << project_receipt.value().source.revision.value()
              << " mesh=" << source_garment->mesh_asset
              << " output_mesh=" << output_mesh_asset
              << " settled_frame=" << project_receipt.value().frame_index
              << " simulation_digest=" << simulation.value().artifact_digest.hex()
              << " output_digest=" << project_receipt.value().output.content_digest.hex()
              << " receipt_digest=" << project_receipt.value().receipt_digest.hex()
              << " package=" << std::quoted(output_root.string()) << '\n';
    return EXIT_SUCCESS;
}

int inspect_ambient_occlusion_bake(
    const std::filesystem::path& project_path,
    std::string_view mesh_id_text,
    const std::filesystem::path& output_root) {
    using namespace carto;
    const auto mesh_asset = parse_mesh_asset_id_argument(mesh_id_text);
    if (!mesh_asset) {
        print_error(mesh_asset.error());
        return EXIT_FAILURE;
    }
    const auto document = project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto mesh = document.value().meshes().find(mesh_asset.value());
    if (mesh == document.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "AO bake mesh asset does not exist in the project"));
        return EXIT_FAILURE;
    }
    const auto ledger_text = read_bounded_text(
        output_root / "receipts.carto-production-receipts",
        production::ProductionReceiptLedger::max_serialized_bytes,
        "AO bake receipt ledger");
    if (!ledger_text) {
        print_error(ledger_text.error());
        return EXIT_FAILURE;
    }
    const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_text.value());
    if (!ledger || ledger.value().size() != 1U) {
        print_error(ledger ? core::Diagnostic(core::ErrorCode::validation_failed,
            "AO bake receipt ledger must contain exactly one record") : ledger.error());
        return EXIT_FAILURE;
    }
    const production::ProductionArtifactReceipt& receipt = ledger.value().records().front();
    const bool d3d12_profile = receipt.key.platform_profile.rfind(
        "carto.d3d12.", 0U) == 0U;
    if (receipt.status != production::ReceiptStatus::admitted ||
        receipt.key.kind != production::ArtifactKind::baked_texture ||
        receipt.key.algorithm != production::kBakeAmbientOcclusionAlgorithm ||
        (!d3d12_profile && receipt.key.platform_profile != "carto.cpu-reference.v1")) {
        print_error(core::Diagnostic(core::ErrorCode::validation_failed,
            "receipt ledger does not contain one admitted CPU or D3D12 AO artifact"));
        return EXIT_FAILURE;
    }

    try {
        assets::BlobStore blobs(output_root / "blobs");
        const auto payload = blobs.read(receipt.output_digest);
        if (!payload || payload.value().size() != receipt.output_bytes ||
            assets::sha256(payload.value()) != receipt.output_digest) {
            print_error(payload ? core::Diagnostic(core::ErrorCode::validation_failed,
                "AO bake payload disagrees with its production receipt") : payload.error());
            return EXIT_FAILURE;
        }
        const auto artifact = production::AmbientOcclusionMapArtifact::deserialize(
            payload.value());
        if (!artifact) {
            print_error(artifact.error());
            return EXIT_FAILURE;
        }
        if (auto result = production::validate_bake_sources(
                artifact.value().recipe.binding, mesh->second, mesh->second); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        const auto expected_key = production::bake_ambient_occlusion_cook_key(
            artifact.value().recipe, receipt.key.platform_profile);
        if (!expected_key || expected_key.value() != receipt.key ||
            artifact.value().recipe.binding.source.identity !=
                document.value().source_namespace() + "/mesh/" +
                    std::to_string(mesh_asset.value())) {
            print_error(expected_key ? core::Diagnostic(core::ErrorCode::stale_data,
                "AO bake recipe or source identity does not match the current project")
                : expected_key.error());
            return EXIT_FAILURE;
        }
        std::cout << "verified"
                  << " kind=ambient-occlusion"
                  << " evidence=" << (d3d12_profile
                      ? "carto.d3d12.cpu-reference-parity" : "carto.cpu-reference")
                  << " platform_profile=" << std::quoted(receipt.key.platform_profile)
                  << " source=" << std::quoted(receipt.key.source.identity)
                  << " source_revision=" << receipt.key.source.revision.value()
                  << " source_digest=" << receipt.key.source.content_digest.hex()
                  << " mesh=" << mesh_asset.value()
                  << " uv_set=" << artifact.value().recipe.binding.target_uv_set
                  << " covered_texels=" << artifact.value().covered_texels
                  << " rays_cast=" << artifact.value().rays_cast
                  << " artifact_digest=" << artifact.value().artifact_digest.hex()
                  << " payload_digest=" << receipt.output_digest.hex()
                  << " receipt_digest=" << receipt.receipt_digest.hex()
                  << '\n';
    } catch (const std::exception& error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            std::string("AO bake verification failed: ") + error.what()));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#if CARTO_CLI_HAS_D3D12_BAKE
int bake_tangent_normal_map_d3d12(char** argv) {
    using namespace carto;
    const std::filesystem::path project_path(argv[2]);
    const auto source_asset = parse_mesh_asset_id_argument(argv[3]);
    const auto target_asset = parse_mesh_asset_id_argument(argv[4]);
    const auto uv_set = parse_bake_u32_argument(argv[5], "UV set id");
    const auto width = parse_bake_u32_argument(argv[6], "texture width");
    const auto height = parse_bake_u32_argument(argv[7], "texture height");
    const auto tile_u = parse_bake_u32_argument(argv[8], "tile U");
    const auto tile_v = parse_bake_u32_argument(argv[9], "tile V");
    const auto half_extent = parse_bake_positive_double_argument(
        argv[10], "projection half extent");
    const std::string_view reject_misses_text(argv[11]);
    const std::filesystem::path output_root(argv[12]);
    if (!source_asset || !target_asset || !uv_set || !width || !height ||
        !tile_u || !tile_v || !half_extent ||
        (reject_misses_text != "0" && reject_misses_text != "1")) {
        const core::Diagnostic error = !source_asset ? source_asset.error()
            : !target_asset ? target_asset.error()
            : !uv_set ? uv_set.error()
            : !width ? width.error()
            : !height ? height.error()
            : !tile_u ? tile_u.error()
            : !tile_v ? tile_v.error()
            : !half_extent ? half_extent.error()
            : core::Diagnostic(core::ErrorCode::invalid_argument,
                "reject-misses must be 0 or 1");
        print_error(error);
        return EXIT_FAILURE;
    }
    if (output_root.empty() || output_root.filename().empty()) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_argument,
            "bake output directory must name a new directory"));
        return EXIT_FAILURE;
    }

    const auto document = project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto source = document.value().meshes().find(source_asset.value());
    const auto target = document.value().meshes().find(target_asset.value());
    if (source == document.value().meshes().end() ||
        target == document.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "normal bake source or target mesh does not exist in the project"));
        return EXIT_FAILURE;
    }

    const std::string source_identity = document.value().source_namespace() +
        "/mesh/" + std::to_string(source_asset.value());
    const std::string target_identity = document.value().source_namespace() +
        "/mesh/" + std::to_string(target_asset.value());
    const auto source_binding = production::bind_bake_source(source_identity, source->second);
    const auto target_binding = production::bind_bake_target(
        target_identity, target->second, uv_set.value());
    if (!source_binding || !target_binding) {
        print_error(!source_binding ? source_binding.error() : target_binding.error());
        return EXIT_FAILURE;
    }

    production::BakeRecipe recipe;
    recipe.identity = "bake/tangent-normal/source/" +
        std::to_string(source_asset.value()) + "/target/" +
        std::to_string(target_asset.value()) + "/uv/" + std::to_string(uv_set.value()) +
        "/tile/" + std::to_string(tile_u.value()) + "/" + std::to_string(tile_v.value());
    recipe.source = source_binding.value();
    recipe.target = target_binding.value();
    recipe.target_uv_set = uv_set.value();
    recipe.width = width.value();
    recipe.height = height.value();
    recipe.tile_u = tile_u.value();
    recipe.tile_v = tile_v.value();
    recipe.projection_half_extent = half_extent.value();
    recipe.reject_projection_misses = reject_misses_text == "1";
    if (auto result = recipe.validate(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) {
        print_error(recipe_digest.error());
        return EXIT_FAILURE;
    }

    const auto device_result = d3d12::D3D12Device::create();
    if (!device_result) {
        print_error(device_result.error().with_context("D3D12 bake worker"));
        return EXIT_FAILURE;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto report = d3d12::compile_tangent_normal_map_d3d12(
        *device_result.value(), recipe, source->second, target->second);
    const auto finished = std::chrono::steady_clock::now();
    for (const auto& diagnostic : report.bake.diagnostics) {
        std::cerr << "diagnostic code=" << diagnostic.code
                  << " message=" << std::quoted(diagnostic.message) << '\n';
    }
    if (!report.succeeded() || !report.projection.has_value() ||
        !report.cpu_reference_artifact_digest.has_value()) return EXIT_FAILURE;

    const auto current_document = project::ProjectDocument::load(project_path);
    if (!current_document) {
        print_error(current_document.error().with_context(
            "reloading project before D3D12 bake publication"));
        return EXIT_FAILURE;
    }
    if (current_document.value().source_namespace() !=
        document.value().source_namespace()) {
        print_error(core::Diagnostic(core::ErrorCode::stale_data,
            "project source namespace changed while the D3D12 bake was running"));
        return EXIT_FAILURE;
    }
    const auto current_source = current_document.value().meshes().find(source_asset.value());
    const auto current_target = current_document.value().meshes().find(target_asset.value());
    if (current_source == current_document.value().meshes().end() ||
        current_target == current_document.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::stale_data,
            "project source or target mesh disappeared while the D3D12 bake was running"));
        return EXIT_FAILURE;
    }
    if (auto result = production::validate_bake_sources(
            recipe, current_source->second, current_target->second); !result) {
        print_error(result.error().with_context(
            "revalidating project meshes before D3D12 bake publication"));
        return EXIT_FAILURE;
    }

    const auto platform_profile_result = d3d12::d3d12_bake_platform_profile(
        *report.projection);
    if (!platform_profile_result) {
        print_error(platform_profile_result.error());
        return EXIT_FAILURE;
    }
    const std::string platform_profile = platform_profile_result.value();
    const auto payload = report.bake.artifact->serialize();
    const auto cook_key = production::bake_production_cook_key(
        recipe, production::BakeNormalSpace::tangent, platform_profile);
    if (!payload || !cook_key) {
        print_error(!payload ? payload.error() : cook_key.error());
        return EXIT_FAILURE;
    }
    production::ProductionArtifactReceipt receipt;
    receipt.key = cook_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = payload.value().size();
    receipt.quality.measured_error = 0.0;
    receipt.quality.importance = 1.0;
    receipt.quality.confidence = 1.0;
    receipt.cost.cook_milliseconds = std::chrono::duration<double, std::milli>(
        finished - started).count();
    receipt.cost.gpu_milliseconds = static_cast<double>(
        report.projection->wall_time_nanoseconds) / 1'000'000.0;
    receipt.cost.disk_bytes = receipt.output_bytes;
    if (auto result = receipt.refresh_digest(); !result) {
        print_error(result.error());
        return EXIT_FAILURE;
    }

    std::error_code output_error;
    const auto parent = output_root.parent_path();
    if (!parent.empty() &&
        (!std::filesystem::exists(parent, output_error) || output_error ||
         !std::filesystem::is_directory(parent, output_error) || output_error)) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            "bake output parent directory is unavailable"));
        return EXIT_FAILURE;
    }
    output_error.clear();
    if (std::filesystem::exists(output_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "bake output directory refuses to overwrite an existing path"));
        return EXIT_FAILURE;
    }
    std::filesystem::path staging_root = output_root;
    staging_root += ".pending-" + recipe_digest.value().hex().substr(0U, 16U);
    output_error.clear();
    if (std::filesystem::exists(staging_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::invalid_state,
            "bake staging directory already exists and requires inspection"));
        return EXIT_FAILURE;
    }
    output_error.clear();
    if (!std::filesystem::create_directory(staging_root, output_error) || output_error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new bake staging directory"));
        return EXIT_FAILURE;
    }

    try {
        assets::BlobStore blobs(staging_root / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        const production::AdmissionPolicy policy;
        if (auto result = d3d12::publish_tangent_normal_map_d3d12(
                artifact_store, receipt, report, current_source->second,
                current_target->second,
                policy, platform_profile); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        const auto reopened = production::read_normal_map(
            artifact_store, recipe, production::BakeNormalSpace::tangent,
            current_source->second, current_target->second, policy, platform_profile);
        if (!reopened) {
            print_error(reopened.error());
            return EXIT_FAILURE;
        }
        if (reopened.value().artifact_digest != report.bake.artifact->artifact_digest) {
            print_error(core::Diagnostic(core::ErrorCode::validation_failed,
                "published D3D12 normal artifact does not match the compiled artifact"));
            return EXIT_FAILURE;
        }
        const std::string ledger_text = artifact_store.ledger().serialize();
        const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_text);
        if (!ledger || ledger.value().size() != 1U) {
            print_error(ledger ? core::Diagnostic(core::ErrorCode::validation_failed,
                "D3D12 bake receipt ledger did not reopen with exactly one record")
                : ledger.error());
            return EXIT_FAILURE;
        }
        const auto manifest_write = write_new_text_atomically(
            staging_root / "receipts.carto-production-receipts", ledger_text,
            "D3D12 bake receipt ledger");
        if (!manifest_write) {
            print_error(manifest_write.error());
            return EXIT_FAILURE;
        }
        const auto reopened_manifest = read_bounded_text(
            staging_root / "receipts.carto-production-receipts",
            production::ProductionReceiptLedger::max_serialized_bytes,
            "D3D12 bake receipt ledger");
        if (!reopened_manifest) {
            print_error(reopened_manifest.error());
            return EXIT_FAILURE;
        }
        const auto verified_manifest = production::ProductionReceiptLedger::deserialize(
            reopened_manifest.value());
        if (!verified_manifest || verified_manifest.value().size() != 1U ||
            verified_manifest.value().records().front().receipt_digest !=
                receipt.receipt_digest) {
            print_error(verified_manifest ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                "D3D12 bake receipt manifest failed read-back validation")
                : verified_manifest.error());
            return EXIT_FAILURE;
        }
        output_error.clear();
        std::filesystem::rename(staging_root, output_root, output_error);
        if (output_error) {
            print_error(core::Diagnostic(core::ErrorCode::io_error,
                "atomic bake output publication failed; inspect the staging directory"));
            return EXIT_FAILURE;
        }
        std::cout << "baked"
                  << " kind=tangent-normal"
                  << " evidence=carto.d3d12.cpu-reference-parity"
                  << " source=" << std::quoted(recipe.source.identity)
                  << " source_revision=" << recipe.source.revision.value()
                  << " source_digest=" << recipe.source.content_digest.hex()
                  << " target=" << std::quoted(recipe.target.identity)
                  << " target_revision=" << recipe.target.revision.value()
                  << " target_digest=" << recipe.target.content_digest.hex()
                  << " uv_set=" << recipe.target_uv_set
                  << " tile=" << recipe.tile_u << ',' << recipe.tile_v
                  << " covered_texels=" << report.bake.artifact->covered_texels
                  << " projected_texels=" << report.bake.artifact->projected_texels
                  << " projection_misses=" << report.bake.artifact->projection_misses
                  << " cpu_reference_digest=" <<
                        report.cpu_reference_artifact_digest->hex()
                  << " gpu_adapter=" << std::quoted(report.projection->adapter_name)
                  << " gpu_adapter_id=" << std::quoted(report.projection->stable_adapter_id)
                  << " gpu_driver=" << std::quoted(report.projection->driver)
                  << " gpu_wall_nanoseconds=" <<
                        report.projection->wall_time_nanoseconds
                  << " platform_profile=" << platform_profile
                  << " artifact_digest=" << report.bake.artifact->artifact_digest.hex()
                  << " payload_digest=" << receipt.output_digest.hex()
                  << " receipt_digest=" << receipt.receipt_digest.hex()
                  << " output=" << std::quoted(output_root.string()) << '\n';
    } catch (const std::exception& error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            std::string("D3D12 bake publication failed: ") + error.what()));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int inspect_tangent_normal_map_d3d12(
    const std::filesystem::path& project_path,
    std::string_view source_id_text,
    std::string_view target_id_text,
    const std::filesystem::path& output_root) {
    using namespace carto;
    const auto source_asset = parse_mesh_asset_id_argument(source_id_text);
    const auto target_asset = parse_mesh_asset_id_argument(target_id_text);
    if (!source_asset || !target_asset) {
        print_error(!source_asset ? source_asset.error() : target_asset.error());
        return EXIT_FAILURE;
    }
    const auto document = project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    const auto source = document.value().meshes().find(source_asset.value());
    const auto target = document.value().meshes().find(target_asset.value());
    if (source == document.value().meshes().end() ||
        target == document.value().meshes().end()) {
        print_error(core::Diagnostic(core::ErrorCode::not_found,
            "normal bake source or target mesh does not exist in the project"));
        return EXIT_FAILURE;
    }
    const auto ledger_text = read_bounded_text(
        output_root / "receipts.carto-production-receipts",
        production::ProductionReceiptLedger::max_serialized_bytes,
        "D3D12 bake receipt ledger");
    if (!ledger_text) {
        print_error(ledger_text.error());
        return EXIT_FAILURE;
    }
    const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_text.value());
    if (!ledger || ledger.value().size() != 1U) {
        print_error(ledger ? core::Diagnostic(core::ErrorCode::validation_failed,
            "D3D12 bake receipt ledger must contain exactly one record") : ledger.error());
        return EXIT_FAILURE;
    }
    const production::ProductionArtifactReceipt& receipt = ledger.value().records().front();
    if (receipt.status != production::ReceiptStatus::admitted ||
        receipt.key.kind != production::ArtifactKind::baked_texture ||
        receipt.key.algorithm != production::kBakeNormalAlgorithm ||
        receipt.key.platform_profile.rfind("carto.d3d12.", 0U) != 0U) {
        print_error(core::Diagnostic(core::ErrorCode::validation_failed,
            "receipt ledger does not contain one admitted D3D12 tangent-normal artifact"));
        return EXIT_FAILURE;
    }

    try {
        assets::BlobStore blobs(output_root / "blobs");
        const auto payload = blobs.read(receipt.output_digest);
        if (!payload || payload.value().size() != receipt.output_bytes ||
            assets::sha256(payload.value()) != receipt.output_digest) {
            print_error(payload ? core::Diagnostic(core::ErrorCode::validation_failed,
                "D3D12 bake payload disagrees with its production receipt")
                : payload.error());
            return EXIT_FAILURE;
        }
        const auto artifact = production::NormalMapArtifact::deserialize(payload.value());
        if (!artifact) {
            print_error(artifact.error());
            return EXIT_FAILURE;
        }
        const std::string expected_source = document.value().source_namespace() +
            "/mesh/" + std::to_string(source_asset.value());
        const std::string expected_target = document.value().source_namespace() +
            "/mesh/" + std::to_string(target_asset.value());
        if (artifact.value().normal_space != production::BakeNormalSpace::tangent ||
            artifact.value().recipe.source.identity != expected_source ||
            artifact.value().recipe.target.identity != expected_target) {
            print_error(core::Diagnostic(core::ErrorCode::stale_data,
                "D3D12 bake source or target identity differs from the current project"));
            return EXIT_FAILURE;
        }
        if (auto result = production::validate_bake_sources(
                artifact.value().recipe, source->second, target->second); !result) {
            print_error(result.error());
            return EXIT_FAILURE;
        }
        const auto expected_key = production::bake_production_cook_key(
            artifact.value().recipe, production::BakeNormalSpace::tangent,
            receipt.key.platform_profile);
        if (!expected_key || expected_key.value() != receipt.key) {
            print_error(expected_key ? core::Diagnostic(core::ErrorCode::stale_data,
                "D3D12 bake receipt key differs from its current source or recipe")
                : expected_key.error());
            return EXIT_FAILURE;
        }
        std::cout << "verified"
                  << " kind=tangent-normal"
                  << " evidence=carto.d3d12.cpu-reference-parity"
                  << " source=" << std::quoted(receipt.key.source.identity)
                  << " source_revision=" << receipt.key.source.revision.value()
                  << " source_digest=" << receipt.key.source.content_digest.hex()
                  << " target=" << std::quoted(artifact.value().recipe.target.identity)
                  << " target_revision=" << artifact.value().recipe.target.revision.value()
                  << " target_digest=" << artifact.value().recipe.target.content_digest.hex()
                  << " uv_set=" << artifact.value().recipe.target_uv_set
                  << " covered_texels=" << artifact.value().covered_texels
                  << " projected_texels=" << artifact.value().projected_texels
                  << " artifact_digest=" << artifact.value().artifact_digest.hex()
                  << " platform_profile=" << receipt.key.platform_profile
                  << " payload_digest=" << receipt.output_digest.hex()
                  << " receipt_digest=" << receipt.receipt_digest.hex() << '\n';
    } catch (const std::exception& error) {
        print_error(core::Diagnostic(core::ErrorCode::io_error,
            std::string("D3D12 bake verification failed: ") + error.what()));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
#endif

int inspect_prepared(const std::filesystem::path& output_root) {
    const auto selected = carto::production::read_selected_prepared_scene(output_root);
    if (!selected) {
        print_error(selected.error());
        return EXIT_FAILURE;
    }
    if (!selected.value().has_value()) {
        std::cerr << "error: no prepared revision is selected\n";
        return EXIT_FAILURE;
    }
    const auto& revision = selected.value().value();
    std::cout << "prepared revision=" << revision.prepared_scene_digest.hex()
              << " source=" << revision.envelope.source_namespace
              << " source_revision=" << revision.envelope.source_revision.value()
              << " profile=" << revision.envelope.profile_identity
              << " assets=" << revision.envelope.assets.size()
              << " nodes=" << revision.envelope.nodes.size()
              << " products=" << revision.envelope.products.size()
              << " bindings=" << revision.envelope.material_bindings.size() << '\n';
    for (const auto& capability : revision.envelope.capabilities) {
        std::cout << "capability feature=" << capability.feature
                  << " evidence=" << static_cast<unsigned>(capability.evidence)
                  << " required=" << (capability.required ? "true" : "false")
                  << " detail=" << std::quoted(capability.detail) << '\n';
    }
    for (const auto& product : revision.envelope.products) {
        std::cout << "product identity=" << product.identity
                  << " kind=" << static_cast<unsigned>(product.kind)
                  << " version=" << product.format_version
                  << " digest=" << product.content_digest.hex()
                  << " path=" << std::quoted(product.relative_path) << '\n';
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
           << "  cartographer_cli export-obj <project.carto> [mesh-asset-id] <mesh.obj>\n"
           << "  cartographer_cli export-ply <project.carto> [mesh-asset-id] <mesh.ply>\n"
           << "  cartographer_cli export-stl <project.carto> [mesh-asset-id] <mesh.stl>\n"
           << "  cartographer_cli export-gltf <project.carto> <scene.gltf>\n"
           << "  cartographer_cli write-default-preparation-profile <profile.carto-prep>\n"
           << "  cartographer_cli inspect-preparation-profile <profile.carto-prep>\n"
           << "  cartographer_cli prepare <project.carto> <output-directory> scene\n"
           << "  cartographer_cli prepare <project.carto> <output-directory> --profile <profile.carto-prep> scene\n"
           << "  cartographer_cli prepare <project.carto> <output-directory> asset <mesh-asset-id>\n"
           << "  cartographer_cli prepare <project.carto> <output-directory> assembly <node-id>\n"
           << "  cartographer_cli prepare <project.carto> <output-directory> selection <node-id>...\n"
           << "  cartographer_cli inspect-prepared <output-directory>\n"
           << "  cartographer_cli bake-ao <project.carto> <mesh-id> <uv-set-id> <width> <height> <tile-u> <tile-v> <samples> <max-distance> <seed> <output-directory> (mesh is both source and UV target; origin bias is 1e-4)\n"
           << "  cartographer_cli inspect-bake-ao <project.carto> <mesh-id> <output-directory>\n"
           << "  cartographer_cli bake-cloth-rest-pose <project.carto> <mesh-id> <frames> <substeps> <iterations> <settle-window> <settle-tolerance> <output-directory> (requires an authored GarmentAsset; gravity and frame time step use CPU-reference defaults)\n"
           << "  cartographer_cli inspect-cloth-rest-pose <output-directory>\n";
#if CARTO_CLI_HAS_D3D12_BAKE
    output << "  cartographer_cli bake-ao-d3d12 <project.carto> <mesh-id> <uv-set-id> <width> <height> <tile-u> <tile-v> <samples> <max-distance> <seed> <output-directory> (mesh is both source and UV target; origin bias is 1e-4)\n"
           << "  cartographer_cli bake-normal-d3d12 <project.carto> <source-mesh-id> <target-mesh-id> <uv-set-id> <width> <height> <tile-u> <tile-v> <projection-half-extent> <reject-misses:0|1> <output-directory>\n"
           << "  cartographer_cli inspect-bake-normal-d3d12 <project.carto> <source-mesh-id> <target-mesh-id> <output-directory>\n";
#endif
    output << "  cartographer_cli import-obj <mesh.obj> <project.carto>\n"
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
    if (command == "export-obj" && (argc == 4 || argc == 5)) {
        if (argc == 4) return export_obj(argv[2], argv[3], std::nullopt);
        const auto mesh_asset = parse_mesh_asset_id_argument(argv[3]);
        if (!mesh_asset) {
            print_error(mesh_asset.error());
            return EXIT_FAILURE;
        }
        return export_obj(argv[2], argv[4], mesh_asset.value());
    }
    if (command == "export-ply" && (argc == 4 || argc == 5)) {
        if (argc == 4) return export_ply(argv[2], argv[3], std::nullopt);
        const auto mesh_asset = parse_mesh_asset_id_argument(argv[3]);
        if (!mesh_asset) {
            print_error(mesh_asset.error());
            return EXIT_FAILURE;
        }
        return export_ply(argv[2], argv[4], mesh_asset.value());
    }
    if (command == "export-stl" && (argc == 4 || argc == 5)) {
        if (argc == 4) return export_stl(argv[2], argv[3], std::nullopt);
        const auto mesh_asset = parse_mesh_asset_id_argument(argv[3]);
        if (!mesh_asset) {
            print_error(mesh_asset.error());
            return EXIT_FAILURE;
        }
        return export_stl(argv[2], argv[4], mesh_asset.value());
    }
    if (command == "export-gltf" && argc == 4) {
        return export_gltf(argv[2], argv[3]);
    }
    if (command == "write-default-preparation-profile" && argc == 3) {
        return write_default_preparation_profile(argv[2]);
    }
    if (command == "inspect-preparation-profile" && argc == 3) {
        return inspect_preparation_profile(argv[2]);
    }
    if (command == "prepare" && argc >= 5) {
        return prepare_project(argc, argv);
    }
    if (command == "inspect-prepared" && argc == 3) {
        return inspect_prepared(argv[2]);
    }
    if (command == "bake-ao" && argc == 13) {
        return bake_ambient_occlusion(argv);
    }
    if (command == "inspect-bake-ao" && argc == 5) {
        return inspect_ambient_occlusion_bake(argv[2], argv[3], argv[4]);
    }
    if (command == "bake-cloth-rest-pose" && argc == 10) {
        return bake_cloth_rest_pose(argv);
    }
    if (command == "inspect-cloth-rest-pose" && argc == 3) {
        return inspect_cloth_rest_pose(argv[2]);
    }
#if CARTO_CLI_HAS_D3D12_BAKE
    if (command == "bake-ao-d3d12" && argc == 13) {
        return bake_ambient_occlusion(argv, true);
    }
    if (command == "bake-normal-d3d12" && argc == 13) {
        return bake_tangent_normal_map_d3d12(argv);
    }
    if (command == "inspect-bake-normal-d3d12" && argc == 6) {
        return inspect_tangent_normal_map_d3d12(argv[2], argv[3], argv[4], argv[5]);
    }
#endif
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
