#include <carto/archetype/digest.hpp>
#include <carto/archetype/inference.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/capability_profile.hpp>
#include <carto/production/closure.hpp>
#include <carto/production/dependency.hpp>
#include <carto/production/recipe.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace archetype = carto::archetype;

using carto::assets::Sha256Digest;
using namespace carto::production;

constexpr std::size_t kWarmupIterations = 5U;

struct TimingSummary {
    double minimum_microseconds = 0.0;
    double median_microseconds = 0.0;
    double p95_microseconds = 0.0;
    double mean_microseconds = 0.0;
    std::size_t checksum = 0U;
};

struct EvidenceRow {
    std::string section;
    std::string_view name;
    std::size_t iterations = 0U;
    std::size_t units = 0U;
    std::string_view unit_label;
    TimingSummary timing;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

[[nodiscard]] std::string padded_index(std::size_t index) {
    std::string text = std::to_string(index);
    while (text.size() < 4U) text.insert(text.begin(), '0');
    return text;
}

[[nodiscard]] TimingSummary summarize(
    const std::vector<double>& samples,
    std::size_t checksum) {
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double rank) {
        const double position = rank * static_cast<double>(sorted.size() - 1U);
        const std::size_t lower = static_cast<std::size_t>(std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
        const double fraction = position - static_cast<double>(lower);
        return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
    };
    const double total = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    return TimingSummary{
        sorted.front(), percentile(0.50), percentile(0.95),
        total / static_cast<double>(sorted.size()), checksum};
}

template <typename Run>
[[nodiscard]] TimingSummary time_loop(std::size_t iterations, Run&& run) {
    std::vector<double> samples;
    samples.reserve(iterations);
    std::size_t checksum = 0U;
    for (std::size_t index = 0U; index < kWarmupIterations + iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const std::uint64_t units = run();
        const auto finished = std::chrono::steady_clock::now();
        if (index >= kWarmupIterations) {
            checksum += static_cast<std::size_t>(units);
            samples.push_back(
                std::chrono::duration<double, std::micro>(finished - started).count());
        }
    }
    return summarize(samples, checksum);
}

[[nodiscard]] ProductionCookKey make_cook_key(std::string_view identity) {
    ProductionCookKey key;
    key.source = {std::string(identity), carto::core::Revision{7U}, digest(identity)};
    key.kind = ArtifactKind::render_mesh;
    key.form = ExecutionForm::cached;
    key.algorithm = "surface-closure-v1";
    key.settings_digest = digest("settings-surface-closure-v1");
    key.platform_profile = "handheld.d3d12";
    key.representation_kind = "lod0";
    key.spatial_scope = "grid=world-grid;level=4;x=0..3;y=0..3;z=0..0";
    key.decision_context = {
        "authored", "gameplay", "high-precision", "resident-page"};
    require(static_cast<bool>(key.validate()), "cook key fixture is invalid");
    return key;
}

[[nodiscard]] EvidenceRow bench_cook_key_canonicalization() {
    const ProductionCookKey key = make_cook_key("scene/house");
    constexpr std::size_t kIterations = 20000U;
    EvidenceRow row;
    row.section = "cook-key";
    row.name = "canonicalize";
    row.iterations = kIterations;
    row.unit_label = "canonical_bytes";
    row.timing = time_loop(kIterations, [&key]() -> std::uint64_t {
        return static_cast<std::uint64_t>(key.canonical().size());
    });
    row.units = key.canonical().size();
    return row;
}

[[nodiscard]] EvidenceRow bench_cook_key_validation() {
    const ProductionCookKey key = make_cook_key("scene/house");
    constexpr std::size_t kIterations = 20000U;
    EvidenceRow row;
    row.section = "cook-key";
    row.name = "validate";
    row.iterations = kIterations;
    row.unit_label = "validated_keys";
    row.timing = time_loop(kIterations, [&key]() -> std::uint64_t {
        return static_cast<std::uint64_t>(static_cast<bool>(key.validate()));
    });
    row.units = 1U;
    return row;
}

struct WorldGraphFixture {
    ProductionCookKey changed;
    ProductionCookGraph graph;
    SpatialTileRange dirty;
    std::size_t global_nodes = 0U;
    std::size_t scoped_nodes = 0U;
};

[[nodiscard]] WorldGraphFixture make_world_graph() {
    constexpr std::int64_t kGridWidth = 6U;
    constexpr std::int64_t kGridHeight = 6U;
    WorldGraphFixture fixture;
    fixture.changed = make_cook_key("source/terrain");

    std::vector<ProductionCookKey> terrain_keys;
    terrain_keys.reserve(static_cast<std::size_t>(kGridWidth * kGridHeight));
    for (std::int64_t y = 0; y < kGridHeight; ++y) {
        for (std::int64_t x = 0; x < kGridWidth; ++x) {
            const SpatialTileRange footprint{
                .grid_identity = "world-grid",
                .level = 4U,
                .minimum_x = x,
                .minimum_y = y,
                .minimum_z = 0,
                .maximum_x = x,
                .maximum_y = y,
                .maximum_z = 0,
            };
            ProductionCookKey terrain = make_cook_key(
                "world/terrain-tile-" + padded_index(static_cast<std::size_t>(x)) +
                "-" + padded_index(static_cast<std::size_t>(y)));
            terrain.spatial_scope = footprint.canonical();
            ProductionCookKey vegetation = make_cook_key(
                "world/vegetation-tile-" + padded_index(static_cast<std::size_t>(x)) +
                "-" + padded_index(static_cast<std::size_t>(y)));
            vegetation.spatial_scope = footprint.canonical();
            vegetation.kind = ArtifactKind::material_package;
            vegetation.algorithm = "vegetation-scatter-v1";
            vegetation.representation_kind.clear();
            require(
                static_cast<bool>(fixture.graph.add_spatial_node(
                    terrain, footprint, {fixture.changed})),
                "terrain tile enters the cook graph");
            require(
                static_cast<bool>(fixture.graph.add_spatial_node(
                    vegetation, footprint, {terrain})),
                "vegetation tile enters the cook graph");
            terrain_keys.push_back(terrain);
        }
    }

    ProductionCookKey navigation = make_cook_key("world/navigation-global");
    navigation.kind = ArtifactKind::residency_page;
    navigation.algorithm = "navigation-weave-v1";
    navigation.representation_kind.clear();
    navigation.spatial_scope.clear();
    require(
        static_cast<bool>(fixture.graph.add_node(navigation, terrain_keys)),
        "global navigation product depends on every terrain tile");
    require(
        static_cast<bool>(fixture.graph.validate()), "cook graph fixture is invalid");

    fixture.dirty = SpatialTileRange{
        .grid_identity = "world-grid",
        .level = 4U,
        .minimum_x = 1,
        .minimum_y = 1,
        .minimum_z = 0,
        .maximum_x = 1,
        .maximum_y = 1,
        .maximum_z = 0,
    };
    const auto global = fixture.graph.invalidation_order(fixture.changed);
    const auto scoped = fixture.graph.invalidation_order(fixture.changed, fixture.dirty);
    require(global && scoped, "both invalidation queries succeed on the fixture");
    fixture.global_nodes = global.value().size();
    fixture.scoped_nodes = scoped.value().size();
    require(
        fixture.scoped_nodes < fixture.global_nodes,
        "localized invalidation must touch fewer nodes than global invalidation");
    return fixture;
}

[[nodiscard]] std::vector<EvidenceRow> bench_cook_invalidation(
    const WorldGraphFixture& fixture) {
    constexpr std::size_t kIterations = 2000U;
    std::vector<EvidenceRow> rows;

    EvidenceRow global;
    global.section = "cook-graph";
    global.name = "invalidation.global";
    global.iterations = kIterations;
    global.unit_label = "invalidated_nodes";
    global.timing = time_loop(kIterations, [&fixture]() -> std::uint64_t {
        const auto order = fixture.graph.invalidation_order(fixture.changed);
        require(static_cast<bool>(order), "global invalidation failed");
        return static_cast<std::uint64_t>(order.value().size());
    });
    global.units = fixture.global_nodes;
    rows.push_back(global);

    EvidenceRow scoped;
    scoped.section = "cook-graph";
    scoped.name = "invalidation.spatial-scoped";
    scoped.iterations = kIterations;
    scoped.unit_label = "invalidated_nodes";
    scoped.timing = time_loop(kIterations, [&fixture]() -> std::uint64_t {
        const auto order =
            fixture.graph.invalidation_order(fixture.changed, fixture.dirty);
        require(static_cast<bool>(order), "scoped invalidation failed");
        return static_cast<std::uint64_t>(order.value().size());
    });
    scoped.units = fixture.scoped_nodes;
    rows.push_back(scoped);
    return rows;
}

[[nodiscard]] DestinationCapabilityProfile make_destination_profile() {
    DestinationCapabilityProfile profile;
    profile.identity = "consumer.destination.handheld.v1";
    profile.profile_revision = carto::core::Revision{2U};
    profile.capabilities.push_back(DestinationCapability{
        artifact_capability_feature(ArtifactKind::material_package),
        PreparedCapabilityEvidence::native_realization,
        "native material graph realized on device"});
    profile.capabilities.push_back(DestinationCapability{
        artifact_capability_feature(ArtifactKind::render_mesh),
        PreparedCapabilityEvidence::runtime_validated,
        "render mesh path validated on device"});
    require(static_cast<bool>(profile.validate()), "fixture profile is invalid");
    return profile;
}

[[nodiscard]] RecipeProductRequest make_product(
    ArtifactKind kind,
    std::string algorithm,
    std::string representation) {
    RecipeProductRequest request;
    request.kind = kind;
    request.algorithm = std::move(algorithm);
    request.settings_digest = digest("settings-" + request.algorithm);
    request.representation_kind = std::move(representation);
    require(
        static_cast<bool>(request.validate()), "fixture product request is invalid");
    return request;
}

[[nodiscard]] AuthoritativeSource make_source(std::string identity) {
    AuthoritativeSource source;
    source.identity = std::move(identity);
    source.revision = carto::core::Revision{4U};
    source.content_digest = digest("content-" + source.identity);
    require(static_cast<bool>(source.validate()), "fixture source is invalid");
    return source;
}

[[nodiscard]] ProductionRecipe make_recipe(
    std::string identity,
    const Sha256Digest& profile_digest,
    const std::vector<AuthoritativeSource>& sources,
    std::vector<RecipeProductRequest> products,
    PreparationScopeKind scope_kind,
    std::vector<std::uint64_t> scope_ids) {
    ProductionRecipe recipe;
    recipe.identity = std::move(identity);
    recipe.source_namespace = "torture.world";
    recipe.source_revision = carto::core::Revision{4U};
    recipe.scope.kind = scope_kind;
    recipe.scope.expected_source_revision = carto::core::Revision{4U};
    recipe.scope.asset_ids = scope_ids;
    recipe.sources = sources;
    recipe.products = std::move(products);
    recipe.destination_profile_identity = "consumer.destination.handheld.v1";
    recipe.destination_profile_digest = profile_digest;
    recipe.budget_policy_digest = digest("budget-policy-v1");
    recipe.form = ExecutionForm::cached;
    recipe.platform_profile = "handheld.d3d12";
    require(static_cast<bool>(recipe.validate()), "fixture recipe is invalid");
    return recipe;
}

[[nodiscard]] std::vector<EvidenceRow> bench_recipe_expansion(
    const DestinationCapabilityProfile& profile) {
    const auto profile_digest = profile.canonical_digest();
    require(static_cast<bool>(profile_digest), "profile digest computes");

    std::vector<AuthoritativeSource> full_sources;
    constexpr std::size_t kFullSourceCount = 512U;
    full_sources.reserve(kFullSourceCount);
    for (std::size_t index = 0U; index < kFullSourceCount; ++index) {
        full_sources.push_back(make_source("world.mesh." + padded_index(index)));
    }
    std::vector<RecipeProductRequest> full_products;
    full_products.push_back(
        make_product(ArtifactKind::material_package, "cook.material-v1", ""));
    full_products.push_back(
        make_product(ArtifactKind::material_package, "cook.material-v2", ""));
    full_products.push_back(
        make_product(ArtifactKind::render_mesh, "cook.static-lod0", "lod0"));
    full_products.push_back(
        make_product(ArtifactKind::render_mesh, "cook.static-lod2", "lod2"));
    const ProductionRecipe full_scope = make_recipe(
        "recipe.full-scope.v1", profile_digest.value(), full_sources,
        std::move(full_products), PreparationScopeKind::scene, {});

    std::vector<AuthoritativeSource> demand_sources;
    demand_sources.push_back(make_source("world.mesh.0001"));
    demand_sources.push_back(make_source("world.mesh.0002"));
    demand_sources.push_back(make_source("world.mesh.0003"));
    std::vector<RecipeProductRequest> demand_products;
    demand_products.push_back(
        make_product(ArtifactKind::render_mesh, "cook.static-lod0", "lod0"));
    const ProductionRecipe demand = make_recipe(
        "recipe.demand.v1", profile_digest.value(), std::move(demand_sources),
        std::move(demand_products), PreparationScopeKind::selection,
        {101U, 102U, 103U});

    const auto full_probe = expand_production_recipe(full_scope, profile);
    const auto demand_probe = expand_production_recipe(demand, profile);
    require(
        full_probe && full_probe.value().size() == kFullSourceCount * 4U,
        "full-scope expansion crosses every source with every product");
    require(
        demand_probe && demand_probe.value().size() == 3U,
        "demand expansion emits exactly the demanded cook keys");

    std::vector<EvidenceRow> rows;
    EvidenceRow full_row;
    full_row.section = "recipe";
    full_row.name = "expansion.full-scope";
    full_row.iterations = 60U;
    full_row.unit_label = "cook_keys";
    full_row.timing = time_loop(full_row.iterations, [&]() -> std::uint64_t {
        const auto keys = expand_production_recipe(full_scope, profile);
        require(static_cast<bool>(keys), "full-scope expansion failed");
        return static_cast<std::uint64_t>(keys.value().size());
    });
    full_row.units = full_probe.value().size();
    rows.push_back(full_row);

    EvidenceRow demand_row;
    demand_row.section = "recipe";
    demand_row.name = "expansion.demand";
    demand_row.iterations = 2000U;
    demand_row.unit_label = "cook_keys";
    demand_row.timing = time_loop(demand_row.iterations, [&]() -> std::uint64_t {
        const auto keys = expand_production_recipe(demand, profile);
        require(static_cast<bool>(keys), "demand expansion failed");
        return static_cast<std::uint64_t>(keys.value().size());
    });
    demand_row.units = demand_probe.value().size();
    rows.push_back(demand_row);
    return rows;
}

[[nodiscard]] ProductionArtifactReceipt make_admitted_receipt() {
    ProductionArtifactReceipt receipt;
    receipt.key.source = {"scene/house", carto::core::Revision{7U}, digest("source-v7")};
    receipt.key.kind = ArtifactKind::render_mesh;
    receipt.key.form = ExecutionForm::cached;
    receipt.key.algorithm = "surface-closure-v1";
    receipt.key.settings_digest = digest("settings");
    receipt.key.platform_profile = "cpu-reference";
    receipt.status = ReceiptStatus::admitted;
    receipt.output_digest = digest("render-mesh-output");
    receipt.output_bytes = 4096U;
    receipt.quality = {.measured_error = 0.02, .importance = 0.9, .confidence = 0.98};
    receipt.cost = {.cook_milliseconds = 4.5,
                    .cpu_milliseconds = 0.6,
                    .gpu_milliseconds = 0.0,
                    .resident_bytes = 8192U,
                    .disk_bytes = 4096U};
    require(
        static_cast<bool>(receipt.refresh_digest()),
        "admitted receipt refreshes its digest");
    return receipt;
}

[[nodiscard]] AdmissionPolicy make_admission_policy() {
    AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = std::nullopt,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = std::nullopt,
    };
    require(static_cast<bool>(policy.validate()), "admission policy is invalid");
    return policy;
}

[[nodiscard]] std::vector<EvidenceRow> bench_admission() {
    const ProductionArtifactReceipt receipt = make_admitted_receipt();
    const AdmissionPolicy policy = make_admission_policy();
    AuthoritativeSource stale_source = receipt.key.source;
    stale_source.revision = carto::core::Revision{8U};

    constexpr std::size_t kIterations = 20000U;

    std::vector<EvidenceRow> rows;
    EvidenceRow admitted_row;
    admitted_row.section = "admission";
    admitted_row.name = "matching-source";
    admitted_row.iterations = kIterations;
    admitted_row.unit_label = "admitted_receipts";
    admitted_row.timing = time_loop(kIterations, [&]() -> std::uint64_t {
        const auto result = validate_publication(receipt, receipt.key.source, policy);
        require(static_cast<bool>(result), "matching receipt must be admitted");
        return 1U;
    });
    admitted_row.units = admitted_row.timing.checksum;
    rows.push_back(admitted_row);

    EvidenceRow rejected_row;
    rejected_row.section = "admission";
    rejected_row.name = "stale-source-rejected";
    rejected_row.iterations = kIterations;
    rejected_row.unit_label = "rejected_receipts";
    rejected_row.timing = time_loop(kIterations, [&]() -> std::uint64_t {
        const auto result = validate_publication(receipt, stale_source, policy);
        require(
            !result && result.error().code == carto::core::ErrorCode::stale_data,
            "stale receipt must fail closed with stale_data");
        return 1U;
    });
    rejected_row.units = rejected_row.timing.checksum;
    rows.push_back(rejected_row);
    return rows;
}

[[nodiscard]] archetype::StructuralDigest make_structure(std::size_t seed) {
    archetype::AssemblyInput input;
    archetype::AssemblyNodeContent root;
    root.mesh_digest = digest("structure-" + std::to_string(seed) + "-root");
    root.material_regions = {static_cast<std::uint32_t>(10U + seed % 23U)};
    input.nodes.push_back(std::move(root));
    const std::size_t children = seed % 4U;
    for (std::size_t index = 0U; index < children; ++index) {
        archetype::AssemblyNodeContent child;
        child.mesh_digest =
            digest("structure-" + std::to_string(seed) + "-child-" + std::to_string(index));
        child.material_regions = {static_cast<std::uint32_t>(index)};
        input.nodes.push_back(std::move(child));
        input.links.push_back(
            archetype::AssemblyLink{static_cast<std::uint32_t>(index + 1U), 0U});
    }
    const auto structural = archetype::StructuralDigest::compute(input);
    require(static_cast<bool>(structural), "structure digest computes");
    require(
        static_cast<bool>(structural.value().validate()),
        "structure digest validates");
    return structural.value();
}

struct InferenceFixture {
    std::vector<archetype::AssemblyObservation> observations;
    std::size_t repeated_structures = 0U;
    std::size_t unique_structures = 0U;
};

[[nodiscard]] InferenceFixture make_inference_fixture() {
    constexpr std::size_t kRepeatedStructures = 16U;
    constexpr std::size_t kUniqueStructures = 64U;
    constexpr std::size_t kObservationCount = 4096U;
    InferenceFixture fixture;
    fixture.repeated_structures = kRepeatedStructures;
    fixture.unique_structures = kUniqueStructures;
    std::vector<archetype::StructuralDigest> structures;
    structures.reserve(kRepeatedStructures + kUniqueStructures);
    for (std::size_t index = 0U; index < kRepeatedStructures + kUniqueStructures;
         ++index) {
        structures.push_back(make_structure(index));
    }
    fixture.observations.reserve(kObservationCount + kUniqueStructures);
    for (std::size_t index = 0U; index < kObservationCount; ++index) {
        fixture.observations.push_back(archetype::AssemblyObservation{
            "scene/node-" + std::to_string(index), structures[index % kRepeatedStructures]});
    }
    for (std::size_t index = 0U; index < kUniqueStructures; ++index) {
        fixture.observations.push_back(archetype::AssemblyObservation{
            "scene/unique-" + std::to_string(index),
            structures[kRepeatedStructures + index]});
    }
    for (const auto& observation : fixture.observations) {
        require(
            static_cast<bool>(observation.validate()),
            "assembly observation validates");
    }
    return fixture;
}

[[nodiscard]] EvidenceRow bench_archetype_inference(
    const InferenceFixture& fixture) {
    constexpr std::size_t kIterations = 20U;
    const auto probe = archetype::infer_archetypes(
        "benchmark.world", carto::core::Revision{3U}, fixture.observations,
        archetype::InferenceOptions{2U});
    require(static_cast<bool>(probe), "archetype inference succeeds");
    require(
        static_cast<bool>(probe.value().validate()),
        "archetype proposal validates");
    require(
        probe.value().candidates.size() == fixture.repeated_structures,
        "every repeated structure becomes an archetype candidate");
    require(
        probe.value().receipt.unique_structure_count ==
            fixture.repeated_structures + fixture.unique_structures,
        "unique structures stay authoring truth and never become candidates");

    EvidenceRow row;
    row.section = "archetype";
    row.name = "inference.large-world";
    row.iterations = kIterations;
    row.unit_label = "observations";
    row.timing = time_loop(kIterations, [&]() -> std::uint64_t {
        const auto proposal = archetype::infer_archetypes(
            "benchmark.world", carto::core::Revision{3U}, fixture.observations,
            archetype::InferenceOptions{2U});
        require(static_cast<bool>(proposal), "archetype inference failed");
        return static_cast<std::uint64_t>(proposal.value().receipt.observation_count);
    });
    row.units = fixture.observations.size();
    return row;
}

void print_row(const EvidenceRow& row) {
    std::cout << row.section << ',' << row.name << ',' << row.iterations << ','
        << row.units << ',' << row.unit_label << ','
        << row.timing.minimum_microseconds << ',' << row.timing.median_microseconds
        << ',' << row.timing.p95_microseconds << ',' << row.timing.mean_microseconds
        << ',' << row.timing.checksum << '\n';
}

} // namespace

int main() {
    try {
        std::cout << "# Cartographer production cook benchmark "
            "(docuseries 110 large-world materialization and 180 demand-driven cook)\n";
        std::cout << "# timing: host-clock CPU std::chrono::steady_clock, single run; "
            "loop counts in the iterations column\n";
        std::cout << "# no device or GPU timing is claimed anywhere in this output; "
            "receipt gpu_milliseconds stays 0.0\n";
        std::cout << "section,case,iterations,units,unit_label,min_us,median_us,p95_us,"
            "mean_us,checksum\n";
        std::cout << std::fixed << std::setprecision(3);

        std::vector<EvidenceRow> rows;
        rows.push_back(bench_cook_key_canonicalization());
        rows.push_back(bench_cook_key_validation());

        const WorldGraphFixture world = make_world_graph();
        const auto invalidation_rows = bench_cook_invalidation(world);
        rows.insert(rows.end(), invalidation_rows.begin(), invalidation_rows.end());

        const DestinationCapabilityProfile profile = make_destination_profile();
        const auto recipe_rows = bench_recipe_expansion(profile);
        rows.insert(rows.end(), recipe_rows.begin(), recipe_rows.end());

        const auto admission_rows = bench_admission();
        rows.insert(rows.end(), admission_rows.begin(), admission_rows.end());

        const InferenceFixture inference = make_inference_fixture();
        const EvidenceRow inference_row = bench_archetype_inference(inference);
        rows.push_back(inference_row);
        for (const auto& row : rows) print_row(row);

        std::cout << "# evidence cook-graph invalidation: global_nodes="
            << world.global_nodes << " scoped_nodes=" << world.scoped_nodes
            << " reduction="
            << std::setprecision(2)
            << static_cast<double>(world.global_nodes) /
                static_cast<double>(world.scoped_nodes)
            << "x (dirty region covers one tile of a 6x6 world grid)\n";
        std::cout << "# evidence recipe expansion: full_scope=512_sources_x_4_products->"
            "2048_cook_keys demand=3_sources_x_1_product->3_cook_keys\n";
        std::cout << "# evidence archetype inference: observations="
            << inference.observations.size()
            << " repeated_structures=" << inference.repeated_structures
            << " unique_structures=" << inference.unique_structures
            << " (docuseries 110: repetition economy, not expanded entity spam)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
