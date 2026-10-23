#include <carto/geometry/mesh.hpp>
#include <carto/surface/normal_closure.hpp>

#include <functional>
#include <iostream>
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

carto::geometry::EditableMesh grid_mesh() {
    carto::geometry::EditableMesh mesh;
    std::vector<carto::geometry::VertexId> vertices;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            const auto vertex = mesh.add_vertex({static_cast<double>(x), static_cast<double>(y),
                                                 0.05 * static_cast<double>((x + 2 * y) % 3)});
            REQUIRE(vertex);
            vertices.push_back(vertex.value());
        }
    }
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const auto at = [=](int px, int py) {
                return vertices[static_cast<std::size_t>(py * 3 + px)];
            };
            REQUIRE(mesh.add_face({at(x, y), at(x + 1, y), at(x + 1, y + 1)}));
            REQUIRE(mesh.add_face({at(x, y), at(x + 1, y + 1), at(x, y + 1)}));
        }
    }
    return mesh;
}

void normal_closure_is_revision_bound_and_deterministic() {
    const auto compiled = grid_mesh().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{801U}, "grid", "concrete");
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions qem_options;
    qem_options.target_triangles = 6U;
    const auto candidate = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), qem_options);
    REQUIRE(candidate);

    carto::surface::SurfaceNormalClosureOptions options;
    options.normal_bins = 12U;
    options.distribution_metrics.max_bins = 12U;
    options.distribution_metrics.sinkhorn_iterations = 48U;
    const auto first = carto::surface::evaluate_surface_normal_closure(
        compiled.value(), domain.value(), candidate.value(), options);
    const auto second = carto::surface::evaluate_surface_normal_closure(
        compiled.value(), domain.value(), candidate.value(), options);
    REQUIRE(first && second);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().validate(compiled.value(), domain.value(), candidate.value(), options));
    REQUIRE(first.value().source_normal_distribution.probabilities.size() == 12U);
    REQUIRE(first.value().candidate_normal_distribution.probabilities.size() == 12U);
    REQUIRE(first.value().relative_area_error >= 0.0);
    REQUIRE(first.value().distribution_metrics.jensen_shannon_divergence >= 0.0);

    auto forged = first.value();
    forged.digest[0] = forged.digest[0] == '0' ? '1' : '0';
    REQUIRE(!forged.validate(compiled.value(), domain.value(), candidate.value(), options));
}

void normal_closure_rejects_mismatched_or_unbounded_options() {
    const auto compiled = grid_mesh().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{802U});
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions qem_options;
    qem_options.target_triangles = 6U;
    const auto candidate = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), qem_options);
    REQUIRE(candidate);

    carto::surface::SurfaceNormalClosureOptions invalid;
    invalid.normal_bins = 0U;
    REQUIRE(!invalid.validate());
    invalid.normal_bins = 32U;
    invalid.distribution_metrics.max_bins = 16U;
    REQUIRE(!invalid.validate());

    auto stale = domain.value();
    stale.source_revision = carto::core::Revision{55U};
    REQUIRE(!carto::surface::evaluate_surface_normal_closure(
        compiled.value(), stale, candidate.value()));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"normal closure is revision bound and deterministic", normal_closure_is_revision_bound_and_deterministic},
        {"normal closure rejects mismatched or unbounded options", normal_closure_rejects_mismatched_or_unbounded_options},
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
