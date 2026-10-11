#include <carto/geometry/mesh.hpp>
#include <carto/surface/visibility_closure.hpp>

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

carto::geometry::EditableMesh parallel_patches() {
    carto::geometry::EditableMesh mesh;
    const auto a0 = mesh.add_vertex({-0.5, -0.5, 0.0});
    const auto a1 = mesh.add_vertex({0.5, -0.5, 0.0});
    const auto a2 = mesh.add_vertex({0.0, 0.5, 0.0});
    const auto b0 = mesh.add_vertex({-0.5, -0.5, 0.5});
    const auto b1 = mesh.add_vertex({0.5, -0.5, 0.5});
    const auto b2 = mesh.add_vertex({0.0, 0.5, 0.5});
    REQUIRE(a0 && a1 && a2 && b0 && b1 && b2);
    REQUIRE(mesh.add_face({a0.value(), a1.value(), a2.value()}));
    REQUIRE(mesh.add_face({b0.value(), b1.value(), b2.value()}));
    return mesh;
}

void visibility_closure_is_deterministic_and_explicit() {
    const auto compiled = grid_mesh().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{901U}, "grid", "concrete");
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions qem_options;
    qem_options.target_triangles = 6U;
    const auto candidate = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), qem_options);
    REQUIRE(candidate);

    carto::surface::SurfaceVisibilityClosureOptions options;
    options.visibility_bins = 8U;
    options.direction_count = 8U;
    options.max_distance = 10.0;
    options.distribution_metrics.max_bins = 8U;
    options.distribution_metrics.sinkhorn_iterations = 48U;
    const auto first = carto::surface::evaluate_surface_visibility_closure(
        compiled.value(), domain.value(), candidate.value(), options);
    const auto second = carto::surface::evaluate_surface_visibility_closure(
        compiled.value(), domain.value(), candidate.value(), options);
    REQUIRE(first && second);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().validate(compiled.value(), domain.value(), candidate.value(), options));
    REQUIRE(first.value().source_visibility_distribution.probabilities.size() == 8U);
    REQUIRE(first.value().candidate_visibility_distribution.probabilities.size() == 8U);
    REQUIRE(first.value().source_average_visibility >= 0.0);
    REQUIRE(first.value().source_average_visibility <= 1.0);
    REQUIRE(first.value().average_visibility_error >= 0.0);
    REQUIRE(first.value().average_visibility_error <= 1.0);

    auto forged = first.value();
    forged.digest[0] = forged.digest[0] == '0' ? '1' : '0';
    REQUIRE(!forged.validate(compiled.value(), domain.value(), candidate.value(), options));
}

void visibility_closure_rejects_invalid_options_and_stale_input() {
    const auto compiled = grid_mesh().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{902U});
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions qem_options;
    qem_options.target_triangles = 6U;
    const auto candidate = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), qem_options);
    REQUIRE(candidate);

    carto::surface::SurfaceVisibilityClosureOptions invalid;
    invalid.visibility_bins = 0U;
    REQUIRE(!invalid.validate());
    invalid.visibility_bins = 16U;
    invalid.distribution_metrics.max_bins = 8U;
    REQUIRE(!invalid.validate());
    invalid.visibility_bins = 8U;
    invalid.distribution_metrics.max_bins = 8U;
    invalid.direction_count = 0U;
    REQUIRE(!carto::surface::evaluate_surface_visibility_closure(
        compiled.value(), domain.value(), candidate.value(), invalid));

    auto stale = domain.value();
    stale.source_revision = carto::core::Revision{88U};
    REQUIRE(!carto::surface::evaluate_surface_visibility_closure(
        compiled.value(), stale, candidate.value()));
}

void visibility_closure_detects_a_bounded_occluder() {
    const auto compiled = parallel_patches().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{903U});
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions qem_options;
    qem_options.target_triangles = 2U;
    const auto candidate = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), qem_options);
    REQUIRE(candidate);

    carto::surface::SurfaceVisibilityClosureOptions options;
    options.visibility_bins = 4U;
    options.direction_count = 8U;
    options.max_distance = 10.0;
    options.distribution_metrics.max_bins = 4U;
    const auto result = carto::surface::evaluate_surface_visibility_closure(
        compiled.value(), domain.value(), candidate.value(), options);
    REQUIRE(result);
    REQUIRE(result.value().source_average_visibility < 1.0);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"visibility closure is deterministic and explicit", visibility_closure_is_deterministic_and_explicit},
        {"visibility closure rejects invalid options and stale input", visibility_closure_rejects_invalid_options_and_stale_input},
        {"visibility closure detects a bounded occluder", visibility_closure_detects_a_bounded_occluder},
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
