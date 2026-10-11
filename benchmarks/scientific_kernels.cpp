#include <carto/assets/blob_store.hpp>
#include <carto/device/kernel_evidence.hpp>
#include <carto/scientific/model.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace carto::scientific;

constexpr std::size_t kWarmupIterations = 5U;
constexpr std::size_t kMeasuredIterations = 40U;
constexpr std::size_t kStructureCount = 128U;
constexpr std::size_t kStageCount = 16U;
constexpr std::size_t kGeneCount = 256U;
constexpr std::size_t kLineageCount = 100U;

struct TimingSummary {
    double minimum_microseconds = 0.0;
    double median_microseconds = 0.0;
    double p95_microseconds = 0.0;
    double mean_microseconds = 0.0;
    std::size_t checksum = 0U;
};

[[nodiscard]] Provenance benchmark_provenance() {
    return Provenance{
        "benchmark://scientific-kernels",
        "2026.10",
        "CC0-1.0",
        "cartographer-scientific-benchmark",
        "2026-10-03T00:00:00Z",
        std::nullopt,
    };
}

[[nodiscard]] carto::assets::Sha256Digest benchmark_digest(char value) {
    const auto parsed = carto::assets::Sha256Digest::from_hex(std::string(64U, value));
    if (!parsed) throw std::runtime_error(parsed.error().message);
    return parsed.value();
}

[[nodiscard]] ScientificModel make_model() {
    const auto provenance = benchmark_provenance();
    ScientificModel model;

    if (auto result = model.insert_parameter(Parameter{
            ParameterId{1U}, "growth-rate", "1/s", 0.25, 0.0, 1.0, provenance});
        !result) {
        throw std::runtime_error(result.error().message);
    }

    MorphologyModel morphology;
    morphology.organism_identity = "scientific-benchmark-organism";
    morphology.body_plan = "segmented-branching";
    morphology.axes = {"anterior-posterior", "dorsal-ventral", "left-right"};
    morphology.symmetry = "bilateral";
    for (std::size_t index = 0U; index < kStructureCount; ++index) {
        const StructureId id{static_cast<std::uint64_t>(index + 1U)};
        const std::optional<StructureId> parent = index == 0U
            ? std::nullopt
            : std::optional<StructureId>{StructureId{static_cast<std::uint64_t>(index)}};
        if (auto result = morphology.insert_structure(MorphologyStructure{
                id, index == 0U ? "organ" : "segment", parent,
                index % 2U == 0U ? "left" : "right", "stage:0", GeometryBinding{},
                "organ-local", {}, "estimated", provenance});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    if (auto result = model.set_morphology(std::move(morphology)); !result) {
        throw std::runtime_error(result.error().message);
    }

    FunctionalGenome genome;
    genome.schema = "cartographer.functional-genome.v1";
    genome.developmental_program_reference = "program://scientific-benchmark";
    genome.lineage_reference = "lineage://scientific-benchmark/0";
    genome.source_digest = benchmark_digest('a');
    for (std::size_t index = 0U; index < kGeneCount; ++index) {
        const auto id = static_cast<std::uint64_t>(index + 1U);
        if (auto result = genome.insert_gene(FunctionalGene{
                GeneId{id}, "gene-" + std::to_string(id), index % 3U == 0U
                    ? "developmental" : index % 3U == 1U ? "regulatory" : "effector",
                "continuous", static_cast<double>(index) / 256.0, "ratio", 0.0, 1.0,
                "bounded-uniform", "inherited", provenance});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    if (auto result = model.set_functional_genome(std::move(genome)); !result) {
        throw std::runtime_error(result.error().message);
    }

    DevelopmentalProgram program;
    program.schema = "cartographer.developmental-program.v1";
    program.reference = "program://scientific-benchmark";
    program.description = "CPU scientific kernel benchmark program";
    program.source_digest = benchmark_digest('b');
    for (std::size_t index = 0U; index < kStageCount; ++index) {
        const auto stage_id = StageId{static_cast<std::uint64_t>(index + 1U)};
        if (auto result = program.insert_stage(DevelopmentStage{
                stage_id, static_cast<std::uint64_t>(index), "stage-" + std::to_string(index),
                "always", provenance});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    for (std::size_t index = 0U; index < kStructureCount; ++index) {
        const auto id = static_cast<std::uint64_t>(index + 1U);
        const auto structure = StructureId{id};
        const std::vector<StructureId> creates = index == 0U
            ? std::vector<StructureId>{structure}
            : std::vector<StructureId>{};
        const std::vector<StructureId> modifies = index == 0U
            ? std::vector<StructureId>{}
            : std::vector<StructureId>{structure};
        if (auto result = program.insert_instruction(DevelopmentInstruction{
                InstructionId{id}, StageId{static_cast<std::uint64_t>(index % kStageCount + 1U)},
                index == 0U ? "instantiate" : "grow", "morphogenesis", "always",
                {ParameterId{1U}}, creates, modifies, provenance});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    if (auto result = model.set_developmental_program(std::move(program)); !result) {
        throw std::runtime_error(result.error().message);
    }

    DevelopmentalTrace trace;
    trace.program_reference = "program://scientific-benchmark";
    trace.genome_reference = "program://scientific-benchmark";
    trace.program_digest = benchmark_digest('b');
    trace.genome_digest = benchmark_digest('a');
    const auto result_digest = benchmark_digest('c');
    for (std::size_t index = 0U; index < kStructureCount; ++index) {
        const auto id = static_cast<std::uint64_t>(index + 1U);
        if (auto result = trace.insert_entry(DevelopmentTraceEntry{
                TraceId{id}, StageId{static_cast<std::uint64_t>(index % kStageCount + 1U)},
                InstructionId{id}, index == 0U ? std::vector<StructureId>{StructureId{id}}
                                               : std::vector<StructureId>{},
                index == 0U ? std::vector<StructureId>{}
                            : std::vector<StructureId>{StructureId{id}},
                "benchmark://development/" + std::to_string(id), provenance,
                std::nullopt, {0.25}, id, result_digest});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    if (auto result = model.set_developmental_trace(std::move(trace)); !result) {
        throw std::runtime_error(result.error().message);
    }
    if (auto result = model.validate(); !result) {
        throw std::runtime_error(result.error().message);
    }
    return model;
}

[[nodiscard]] LineageGraph make_lineage() {
    const auto provenance = benchmark_provenance();
    LineageGraph lineage;
    for (std::size_t index = 0U; index < kLineageCount; ++index) {
        const auto id = LineageId{static_cast<std::uint64_t>(index + 1U)};
        const std::optional<LineageId> parent = index == 0U
            ? std::nullopt
            : std::optional<LineageId>{LineageId{static_cast<std::uint64_t>(index)}};
        if (auto result = lineage.insert_node(LineageNode{
                id, parent, std::nullopt, static_cast<std::uint64_t>(index),
                std::nullopt, std::nullopt, provenance, "benchmark-node-" + std::to_string(index)});
            !result) {
            throw std::runtime_error(result.error().message);
        }
        if (index == 0U) continue;
        if (auto result = lineage.insert_mutation(MutationReceipt{
                MutationId{static_cast<std::uint64_t>(index)},
                LineageId{static_cast<std::uint64_t>(index)}, id,
                std::nullopt, std::nullopt, static_cast<std::uint64_t>(index),
                "benchmark-mutation", {GeneId{static_cast<std::uint64_t>(index)}}, {}, {},
                "accepted-for-authoring-review", provenance});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    if (auto result = lineage.validate(); !result) {
        throw std::runtime_error(result.error().message);
    }
    return lineage;
}

[[nodiscard]] MorphometricModel make_morphometrics() {
    const auto provenance = benchmark_provenance();
    MorphometricModel morphometrics;
    for (std::size_t index = 0U; index < kLineageCount; ++index) {
        const auto id = static_cast<std::uint64_t>(index + 1U);
        if (auto result = morphometrics.insert_signature(MorphologySignature{
                SignatureId{id}, StructureId{id}, "benchmark-shape", "organ-local", "authored",
                {static_cast<double>(index), static_cast<double>(index * 2U),
                 static_cast<double>(index % 11U)},
                {"major", "minor", "branch"}, provenance});
            !result) {
            throw std::runtime_error(result.error().message);
        }
    }
    if (auto result = morphometrics.validate(); !result) {
        throw std::runtime_error(result.error().message);
    }
    return morphometrics;
}

template <typename Function>
[[nodiscard]] TimingSummary benchmark(Function&& function) {
    std::vector<double> samples;
    samples.reserve(kMeasuredIterations);
    std::size_t checksum = 0U;
    for (std::size_t index = 0U; index < kWarmupIterations + kMeasuredIterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        checksum += function();
        const auto finished = std::chrono::steady_clock::now();
        if (index >= kWarmupIterations) {
            samples.push_back(
                std::chrono::duration<double, std::micro>(finished - started).count());
        }
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double rank) {
        const double position = rank * static_cast<double>(samples.size() - 1U);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = static_cast<std::size_t>(std::ceil(position));
        const double fraction = position - static_cast<double>(lower);
        return samples[lower] + (samples[upper] - samples[lower]) * fraction;
    };
    return TimingSummary{
        samples.front(), percentile(0.50), percentile(0.95),
        std::accumulate(samples.begin(), samples.end(), 0.0) /
            static_cast<double>(samples.size()), checksum};
}

} // namespace

int main() {
    try {
        const std::vector<std::string_view> scientific_kernel_ids{
            "cartographer.scientific.validate",
            "cartographer.scientific.serialize",
            "cartographer.scientific.deserialize",
            "cartographer.scientific.developmental_timeline",
            "cartographer.scientific.developmental_impact",
            "cartographer.scientific.functional_genome_diff",
            "cartographer.scientific.lineage_validate",
            "cartographer.scientific.evolutionary_morphospace",
        };
        for (const auto kernel_id : scientific_kernel_ids) {
            const carto::device::KernelContract contract{
                std::string(kernel_id), "scientific_authoring_preview",
                carto::device::PrecisionRole::output,
                carto::device::KernelDirection::compute,
                "fp64", "fp64", carto::device::BackendKind::cpu, false};
            if (auto result = carto::device::validate(contract); !result) {
                throw std::runtime_error(result.error().message);
            }
        }
        auto model = make_model();
        if (auto result = model.set_morphometrics(make_morphometrics()); !result) {
            throw std::runtime_error(result.error().message);
        }
        if (auto result = model.set_lineage_graph(make_lineage()); !result) {
            throw std::runtime_error(result.error().message);
        }
        if (auto result = model.validate(); !result) {
            throw std::runtime_error(result.error().message);
        }
        const auto serialized = model.serialize();
        const auto parent_genome = *model.functional_genome();
        auto child_genome = parent_genome;
        auto changed_gene = child_genome.genes().at(GeneId{1U});
        changed_gene.value = 0.875;
        if (auto result = child_genome.replace_gene(std::move(changed_gene)); !result) {
            throw std::runtime_error(result.error().message);
        }
        const auto lineage = make_lineage();
        std::vector<EvolutionaryMorphospaceBinding> morphospace_bindings;
        morphospace_bindings.reserve(kLineageCount);
        for (std::size_t index = 0U; index < kLineageCount; ++index) {
            const auto id = static_cast<std::uint64_t>(index + 1U);
            morphospace_bindings.push_back(EvolutionaryMorphospaceBinding{
                LineageId{id}, SignatureId{id}});
        }

        std::cout << "kernel,iterations,structures,instructions,genes,lineage_nodes,bytes,"
                     "min_us,median_us,p95_us,mean_us,checksum\n";
        std::cout << std::fixed << std::setprecision(3);

        const auto print = [&serialized](const char* name, const TimingSummary& summary) {
            std::cout << name << ',' << kMeasuredIterations << ',' << kStructureCount << ','
                << kStructureCount << ',' << kGeneCount << ',' << kLineageCount << ','
                << serialized.size() << ',' << summary.minimum_microseconds << ','
                << summary.median_microseconds << ',' << summary.p95_microseconds << ','
                << summary.mean_microseconds << ',' << summary.checksum << '\n';
        };

        print("cartographer.scientific.validate", benchmark([&model]() {
            return model.validate() ? std::size_t{1U} : std::size_t{0U};
        }));
        print("cartographer.scientific.serialize", benchmark([&model]() {
            return model.serialize().size();
        }));
        print("cartographer.scientific.deserialize", benchmark([&serialized]() {
            const auto restored = ScientificModel::deserialize(serialized);
            if (!restored) throw std::runtime_error(restored.error().message);
            return restored.value().regions().size() +
                restored.value().morphology()->structures().size();
        }));
        print("cartographer.scientific.developmental_timeline", benchmark([&model]() {
            const auto timeline = model.build_developmental_timeline();
            if (!timeline) throw std::runtime_error(timeline.error().message);
            return timeline.value().size();
        }));
        print("cartographer.scientific.developmental_impact", benchmark([&model]() {
            const auto impact = model.build_developmental_instruction_impact(InstructionId{64U});
            if (!impact) throw std::runtime_error(impact.error().message);
            return impact.value().observed_modifies.size() + impact.value().trace_entries.size();
        }));
        print("cartographer.scientific.functional_genome_diff", benchmark([&parent_genome, &child_genome]() {
            const auto diff = child_genome.diff_against(parent_genome);
            return diff.changed.size() + diff.changed_modules.size();
        }));
        print("cartographer.scientific.lineage_validate", benchmark([&lineage]() {
            return lineage.validate() ? lineage.nodes().size() : std::size_t{0U};
        }));
        print("cartographer.scientific.evolutionary_morphospace", benchmark([&model, &morphospace_bindings]() {
            const auto preview = model.preview_evolutionary_morphospace(
                morphospace_bindings, 3U, "unit-box");
            if (!preview) throw std::runtime_error(preview.error().message);
            return preview.value().points.size() + preview.value().trajectories.size();
        }));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scientific benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
