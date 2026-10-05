#include <carto/assets/blob_store.hpp>
#include <carto/surface/budget.hpp>
#include <carto/surface/candidate.hpp>
#include <carto/surface/evidence.hpp>
#include <carto/surface/invalidation.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw TestFailure(std::string("requirement failed: ") + #condition);            \
        }                                                                                   \
    } while (false)

std::string test_digest(std::string value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

carto::surface::SurfaceWorkCandidate work_candidate(
    std::uint64_t id,
    carto::surface::SurfaceWorkKind kind,
    double reduction,
    double semantic,
    double silhouette,
    std::uint64_t bytes) {
    carto::surface::SurfaceWorkCandidate candidate;
    candidate.id = id;
    candidate.domain = carto::surface::SurfaceDomainId{7U};
    candidate.source_revision = carto::core::Revision{3U};
    candidate.kind = kind;
    candidate.uncertainty = 0.8;
    candidate.expected_error_reduction = reduction;
    candidate.semantic_importance = semantic;
    candidate.silhouette_importance = silhouette;
    candidate.estimated_cook_cost = 0.1 * static_cast<double>(id);
    candidate.estimated_runtime_cost = 2.0 * static_cast<double>(id);
    candidate.estimated_bytes = bytes;
    REQUIRE(candidate.refresh_digest());
    return candidate;
}

void candidate_planner_is_deterministic_and_budgeted() {
    const auto first = work_candidate(
        1U, carto::surface::SurfaceWorkKind::refine_silhouette, 0.4, 0.8, 0.2, 128U);
    const auto second = work_candidate(
        2U, carto::surface::SurfaceWorkKind::preserve_feature, 0.2, 0.9, 0.9, 64U);

    carto::surface::SurfaceCandidateOptimizerOptions options;
    options.max_iterations = 4U;
    options.initial_error = 1.0;
    options.target_error = 0.4;
    options.max_bytes = 192U;
    const auto planned = carto::surface::plan_surface_work(
        carto::surface::SurfaceDomainId{7U}, carto::core::Revision{3U}, {second, first}, options);
    REQUIRE(planned);
    REQUIRE(planned.value().steps.size() == 2U);
    REQUIRE(planned.value().steps.front().candidate.id == 1U);
    REQUIRE(planned.value().final_error == 0.4);
    REQUIRE(planned.value().stop_reason ==
            carto::surface::SurfaceOptimizationStopReason::quality_target);
    REQUIRE(planned.value().validate());

    const auto reordered = carto::surface::plan_surface_work(
        carto::surface::SurfaceDomainId{7U}, carto::core::Revision{3U}, {first, second}, options);
    REQUIRE(reordered);
    REQUIRE(reordered.value().digest == planned.value().digest);

    options.max_bytes = 128U;
    const auto bounded = carto::surface::plan_surface_work(
        carto::surface::SurfaceDomainId{7U}, carto::core::Revision{3U}, {first, second}, options);
    REQUIRE(bounded);
    REQUIRE(bounded.value().steps.size() == 1U);
    REQUIRE(bounded.value().stop_reason ==
            carto::surface::SurfaceOptimizationStopReason::budget_exhausted);

    const std::vector<carto::surface::SurfaceWorkCandidate> too_many_candidates(
        carto::surface::SurfaceCandidateOptimizerOptions::max_candidates + 1U);
    REQUIRE(!carto::surface::plan_surface_work(
        carto::surface::SurfaceDomainId{7U}, carto::core::Revision{3U},
        too_many_candidates, options));

    auto forged = first;
    forged.digest.assign(64U, '0');
    REQUIRE(!carto::surface::surface_work_priority(forged));

    auto forged_plan = planned.value();
    forged_plan.steps.front().remaining_error += 0.01;
    REQUIRE(!forged_plan.refresh_digest());
    REQUIRE(!forged_plan.validate());

    auto invalid_kind = first;
    invalid_kind.kind = static_cast<carto::surface::SurfaceWorkKind>(99);
    REQUIRE(!invalid_kind.refresh_digest());
    REQUIRE(!invalid_kind.validate());

    auto invalid_stop = planned.value();
    invalid_stop.stop_reason =
        static_cast<carto::surface::SurfaceOptimizationStopReason>(99);
    REQUIRE(!invalid_stop.refresh_digest());
    REQUIRE(!invalid_stop.validate());
}

carto::surface::SurfaceEvidenceCase evidence_case(
    std::uint64_t id,
    std::string source_digest,
    carto::core::Revision revision) {
    carto::surface::SurfaceEvidenceCase evidence;
    evidence.id = carto::surface::EvidenceCaseId{id};
    evidence.domain = carto::surface::SurfaceDomainId{7U};
    evidence.representation = carto::surface::RepresentationId{11U};
    evidence.view_region = {1U, {0.0, 0.0, 1.0}, 0.15, 8U};
    evidence.light_region = {2U, {1.0, 0.0, 0.0}, 0.25, 8U};
    evidence.measured_error.rms_radiance_error = 0.1 * static_cast<double>(id);
    evidence.dependencies = {
        carto::surface::SurfaceDependencyId{3U},
        carto::surface::SurfaceDependencyId{4U},
    };
    evidence.diagnostic_weight = 2.0;
    evidence.source_revision = revision;
    evidence.source_content_digest = std::move(source_digest);
    REQUIRE(evidence.refresh_digest());
    return evidence;
}

void evidence_reservoir_reuses_only_current_cases() {
    const std::string source_a = test_digest("source-a");
    const std::string source_b = test_digest("source-b");
    const auto original = evidence_case(1U, source_a, carto::core::Revision{3U});
    carto::surface::SurfaceEvidenceReservoir reservoir;
    REQUIRE(reservoir.append(original));

    carto::surface::SurfaceEvidenceQuery query{
        original.domain,
        original.representation,
        original.view_region,
        original.light_region,
        original.source_revision,
        source_a,
        original.dependencies,
    };
    const auto reused = reservoir.query(query);
    REQUIRE(reused && reused.value().has_value());
    REQUIRE(reused.value()->digest == original.digest);

    const auto replacement = evidence_case(1U, source_b, carto::core::Revision{3U});
    REQUIRE(reservoir.append(replacement));
    const auto stale = reservoir.query(query);
    REQUIRE(stale && !stale.value().has_value());

    query.source_content_digest = source_b;
    const auto current = reservoir.query(query);
    REQUIRE(current && current.value().has_value());
    REQUIRE(current.value()->digest == replacement.digest);
    REQUIRE(reservoir.validate());

    query.dependencies.push_back(carto::surface::SurfaceDependencyId{5U});
    REQUIRE(reservoir.query(query));
    REQUIRE(!reservoir.query(query).value().has_value());
}

carto::surface::SurfaceArtifactNode artifact(
    std::uint64_t id,
    std::vector<carto::geometry::FaceId> faces,
    bool geometry,
    bool material = false) {
    return {
        carto::surface::SurfaceArtifactId{id},
        carto::surface::SurfaceDomainId{7U},
        carto::core::Revision{3U},
        {std::move(faces), {}, geometry, material, false, false},
    };
}

void invalidation_is_local_and_transitive() {
    carto::surface::SurfaceInvalidationGraph graph;
    REQUIRE(graph.add_node(artifact(1U, {{10U}}, true)));
    REQUIRE(graph.add_node(artifact(2U, {{11U}}, true)));
    REQUIRE(graph.add_node(artifact(3U, {}, true)));
    REQUIRE(graph.add_node(artifact(4U, {}, false, true)));
    REQUIRE(graph.add_node(artifact(5U, {}, false, true)));
    REQUIRE(graph.add_node(artifact(6U, {}, false, true)));
    carto::surface::SurfaceArtifactNode unrelated_edge;
    unrelated_edge.id = {7U};
    unrelated_edge.domain = {7U};
    unrelated_edge.source_revision = carto::core::Revision{3U};
    unrelated_edge.dependency.edges = {carto::geometry::EdgeId{20U}};
    unrelated_edge.dependency.uses_geometry = true;
    REQUIRE(graph.add_node(unrelated_edge));
    REQUIRE(graph.add_edge({{1U}, {5U}, carto::surface::SurfaceDependencyEdgeType::invalidates}));
    REQUIRE(graph.add_edge({{5U}, {6U}, carto::surface::SurfaceDependencyEdgeType::derived_from}));
    auto invalid_edge = carto::surface::SurfaceDependencyEdge{
        {1U}, {5U}, static_cast<carto::surface::SurfaceDependencyEdgeType>(99)};
    REQUIRE(!graph.add_edge(invalid_edge));

    const carto::surface::SurfaceStateDelta delta{
        carto::surface::SurfaceDomainId{7U},
        carto::core::Revision{3U},
        carto::core::Revision{4U},
        {{10U}},
        {},
        true,
        false,
        false,
        false,
    };
    const auto receipt = graph.invalidate(delta);
    REQUIRE(receipt);
    const std::vector<carto::surface::SurfaceArtifactId> expected_direct{{1U}, {3U}};
    const std::vector<carto::surface::SurfaceArtifactId> expected_invalidated{
        {1U}, {3U}, {5U}, {6U}};
    REQUIRE(receipt.value().directly_affected == expected_direct);
    REQUIRE(receipt.value().invalidated == expected_invalidated);
    REQUIRE(receipt.value().validate());

    auto backward = delta;
    backward.new_revision = carto::core::Revision{2U};
    REQUIRE(!backward.validate());

    auto forged_receipt = receipt.value();
    forged_receipt.invalidated.push_back({});
    REQUIRE(forged_receipt.refresh_digest());
    REQUIRE(!forged_receipt.validate());

    carto::surface::SurfaceInvalidationGraph cyclic;
    REQUIRE(cyclic.add_node(artifact(1U, {{10U}}, true)));
    REQUIRE(cyclic.add_node(artifact(2U, {}, false, true)));
    REQUIRE(cyclic.add_edge({{1U}, {2U}, carto::surface::SurfaceDependencyEdgeType::depends_on}));
    REQUIRE(cyclic.add_edge({{2U}, {1U}, carto::surface::SurfaceDependencyEdgeType::depends_on}));
    REQUIRE(!cyclic.validate());
}

carto::surface::FidelityOption fidelity_option(
    std::uint64_t option_id,
    std::uint64_t domain,
    std::uint64_t representation,
    double quality,
    std::uint64_t bytes,
    std::uint64_t geometry_bytes,
    std::uint64_t closure_bytes) {
    return {
        option_id,
        carto::surface::SurfaceDomainId{domain},
        carto::surface::RepresentationId{representation},
        carto::core::Revision{3U},
        quality,
        5.0 * static_cast<double>(option_id),
        0.25 * static_cast<double>(option_id),
        bytes,
        geometry_bytes,
        closure_bytes,
    };
}

void fidelity_governor_is_scene_wide_and_bounded() {
    const std::vector<carto::surface::FidelityOption> options = {
        fidelity_option(1U, 7U, 11U, 0.60, 100U, 80U, 20U),
        fidelity_option(2U, 7U, 12U, 0.95, 600U, 500U, 100U),
        fidelity_option(3U, 8U, 13U, 0.70, 100U, 90U, 10U),
    };
    const carto::surface::FidelityBudget budget{300U, 220U, 100U, 40.0, 5.0};
    const auto selection = carto::surface::select_fidelity_options(budget, options);
    REQUIRE(selection);
    REQUIRE(selection.value().selected.size() == 2U);
    REQUIRE(selection.value().selected[0].domain == carto::surface::SurfaceDomainId{7U});
    REQUIRE(selection.value().selected[1].domain == carto::surface::SurfaceDomainId{8U});
    REQUIRE(selection.value().total_bytes == 200U);
    REQUIRE(selection.value().total_geometry_bytes == 170U);
    REQUIRE(selection.value().validate());

    auto reversed = options;
    std::reverse(reversed.begin(), reversed.end());
    const auto same = carto::surface::select_fidelity_options(budget, reversed);
    REQUIRE(same && same.value().digest == selection.value().digest);

    auto stale_selection = selection.value();
    stale_selection.source_revision = carto::core::Revision{4U};
    REQUIRE(stale_selection.refresh_digest());
    REQUIRE(!stale_selection.validate());

    auto mixed_revision = options;
    mixed_revision.back().source_revision = carto::core::Revision{4U};
    REQUIRE(!carto::surface::select_fidelity_options(budget, mixed_revision));

    const std::vector<carto::surface::FidelityOption> too_many_options(
        carto::surface::FidelityBudgetSelection::max_options + 1U);
    REQUIRE(!carto::surface::select_fidelity_options(budget, too_many_options));

    auto reordered_selection = selection.value();
    std::reverse(reordered_selection.selected.begin(), reordered_selection.selected.end());
    REQUIRE(reordered_selection.refresh_digest());
    REQUIRE(!reordered_selection.validate());

    auto forged = options.front();
    forged.bytes = 10U;
    REQUIRE(!forged.validate());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"candidate planner is deterministic and budgeted", candidate_planner_is_deterministic_and_budgeted},
        {"evidence reservoir reuses only current cases", evidence_reservoir_reuses_only_current_cases},
        {"invalidation is local and transitive", invalidation_is_local_and_transitive},
        {"fidelity governor is scene-wide and bounded", fidelity_governor_is_scene_wide_and_bounded},
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
