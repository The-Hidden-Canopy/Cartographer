#include <carto/assets/blob_store.hpp>
#include <carto/surface/budget.hpp>
#include <carto/surface/candidate.hpp>
#include <carto/surface/evidence.hpp>
#include <carto/surface/invalidation.hpp>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::string digest(std::string value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

carto::surface::SurfaceWorkCandidate candidate(std::uint64_t id) {
    carto::surface::SurfaceWorkCandidate value;
    value.id = id;
    value.domain = carto::surface::SurfaceDomainId{1U};
    value.source_revision = carto::core::Revision{1U};
    value.kind = id % 2U == 0U
        ? carto::surface::SurfaceWorkKind::refine_silhouette
        : carto::surface::SurfaceWorkKind::increase_ndf_order;
    value.uncertainty = 0.5 + 0.01 * static_cast<double>(id % 20U);
    value.expected_error_reduction = 0.01 + 0.002 * static_cast<double>(id % 10U);
    value.semantic_importance = 0.4 + 0.02 * static_cast<double>(id % 10U);
    value.silhouette_importance = 0.3 + 0.03 * static_cast<double>(id % 10U);
    value.estimated_cook_cost = 0.01 * static_cast<double>(id);
    value.estimated_runtime_cost = 1.0 + static_cast<double>(id);
    value.estimated_bytes = 64U * id;
    require(static_cast<bool>(value.refresh_digest()), "candidate digest failed");
    return value;
}

carto::surface::FidelityOption option(
    std::uint64_t option_id,
    std::uint64_t domain,
    std::uint64_t representation,
    double quality,
    double runtime,
    double cook,
    std::uint64_t bytes,
    std::uint64_t geometry_bytes,
    std::uint64_t closure_bytes) {
    return {
        option_id,
        carto::surface::SurfaceDomainId{domain},
        carto::surface::RepresentationId{representation},
        carto::core::Revision{1U},
        quality,
        runtime,
        cook,
        bytes,
        geometry_bytes,
        closure_bytes,
    };
}

} // namespace

int main() {
    try {
        std::vector<carto::surface::SurfaceWorkCandidate> candidates;
        for (std::uint64_t id = 1U; id <= 64U; ++id) candidates.push_back(candidate(id));
        carto::surface::SurfaceCandidateOptimizerOptions planner_options;
        planner_options.max_iterations = 32U;
        planner_options.initial_error = 1.0;
        planner_options.target_error = 0.2;
        planner_options.max_bytes = 64U * 1024U;

        const auto planner_start = Clock::now();
        const auto plan = carto::surface::plan_surface_work(
            carto::surface::SurfaceDomainId{1U}, carto::core::Revision{1U},
            candidates, planner_options);
        const auto planner_end = Clock::now();
        require(plan && plan.value().validate(), "surface candidate benchmark failed");

        carto::surface::SurfaceEvidenceReservoir reservoir;
        for (std::uint64_t id = 1U; id <= 128U; ++id) {
            carto::surface::SurfaceEvidenceCase evidence;
            evidence.id = carto::surface::EvidenceCaseId{id};
            evidence.domain = carto::surface::SurfaceDomainId{1U};
            evidence.representation = carto::surface::RepresentationId{1U + id % 4U};
            evidence.view_region = {1U + static_cast<std::uint32_t>(id % 4U), {0.0, 0.0, 1.0}, 0.2, 8U};
            evidence.light_region = {1U + static_cast<std::uint32_t>(id % 3U), {1.0, 0.0, 0.0}, 0.3, 8U};
            evidence.source_revision = carto::core::Revision{1U};
            evidence.source_content_digest = digest("benchmark-source");
            evidence.dependencies = {carto::surface::SurfaceDependencyId{1U}};
            evidence.measured_error.rms_radiance_error = 0.1;
            require(static_cast<bool>(evidence.refresh_digest()), "evidence digest failed");
            if (reservoir.size() < carto::surface::SurfaceEvidenceReservoir::max_cases) {
                require(static_cast<bool>(reservoir.append(std::move(evidence))),
                        "evidence append failed");
            }
        }
        carto::surface::SurfaceEvidenceQuery query{
            carto::surface::SurfaceDomainId{1U},
            carto::surface::RepresentationId{2U},
            {2U, {0.0, 0.0, 1.0}, 0.2, 8U},
            {2U, {1.0, 0.0, 0.0}, 0.3, 8U},
            carto::core::Revision{1U},
            digest("benchmark-source"),
            {carto::surface::SurfaceDependencyId{1U}},
        };
        const auto reservoir_start = Clock::now();
        const auto reuse = reservoir.query(query);
        const auto reservoir_end = Clock::now();
        require(reuse && reuse.value().has_value(), "evidence query failed");

        carto::surface::SurfaceInvalidationGraph graph;
        carto::surface::SurfaceArtifactNode geometry_node;
        geometry_node.id = {1U};
        geometry_node.domain = {1U};
        geometry_node.source_revision = carto::core::Revision{1U};
        geometry_node.dependency.faces = {carto::geometry::FaceId{1U}};
        geometry_node.dependency.uses_geometry = true;
        require(static_cast<bool>(graph.add_node(std::move(geometry_node))),
                "invalidation node failed");
        carto::surface::SurfaceArtifactNode material_node;
        material_node.id = {2U};
        material_node.domain = {1U};
        material_node.source_revision = carto::core::Revision{1U};
        material_node.dependency.uses_material = true;
        require(static_cast<bool>(graph.add_node(std::move(material_node))),
                "invalidation node failed");
        const carto::surface::SurfaceDependencyEdge edge{
            {1U}, {2U}, carto::surface::SurfaceDependencyEdgeType::invalidates};
        require(static_cast<bool>(graph.add_edge(edge)), "invalidation edge failed");
        carto::surface::SurfaceStateDelta delta;
        delta.domain = {1U};
        delta.previous_revision = carto::core::Revision{1U};
        delta.new_revision = carto::core::Revision{2U};
        delta.changed_faces = {carto::geometry::FaceId{1U}};
        delta.geometry_changed = true;
        const auto invalidation_start = Clock::now();
        const auto invalidated = graph.invalidate(delta);
        const auto invalidation_end = Clock::now();
        require(invalidated && invalidated.value().invalidated.size() == 2U,
                "invalidation benchmark failed");

        const std::vector<carto::surface::FidelityOption> options = {
            option(1U, 1U, 1U, 0.7, 4.0, 0.2, 1024U, 768U, 256U),
            option(2U, 2U, 2U, 0.6, 3.0, 0.1, 512U, 384U, 128U),
        };
        const carto::surface::FidelityBudget budget{4096U, 3072U, 1024U, 20.0, 2.0};
        const auto budget_start = Clock::now();
        const auto selection = carto::surface::select_fidelity_options(budget, options);
        const auto budget_end = Clock::now();
        require(selection && selection.value().validate(), "budget benchmark failed");

        const auto milliseconds = [](auto start, auto end) {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };
        std::cout << "stage,items,elapsed_ms,digest\n"
                  << std::fixed << std::setprecision(6)
                  << "candidate_plan," << candidates.size() << ','
                  << milliseconds(planner_start, planner_end) << ',' << plan.value().digest << '\n'
                  << "evidence_query," << reservoir.size() << ','
                  << milliseconds(reservoir_start, reservoir_end) << ','
                  << reuse.value()->digest << '\n'
                  << "invalidation," << graph.nodes().size() << ','
                  << milliseconds(invalidation_start, invalidation_end) << ','
                  << invalidated.value().digest << '\n'
                  << "budget_selection," << options.size() << ','
                  << milliseconds(budget_start, budget_end) << ','
                  << selection.value().digest << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "surface workflow benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
