#include <carto/geometry/mesh.hpp>
#include <carto/surface/qem.hpp>

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

carto::geometry::EditableMesh grid_mesh(int side) {
    carto::geometry::EditableMesh mesh;
    std::vector<carto::geometry::VertexId> vertices;
    vertices.reserve(static_cast<std::size_t>(side * side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const auto vertex = mesh.add_vertex({static_cast<double>(x), static_cast<double>(y),
                                                 0.05 * static_cast<double>((x + 2 * y) % 3)});
            REQUIRE(vertex);
            vertices.push_back(vertex.value());
        }
    }
    for (int y = 0; y < side - 1; ++y) {
        for (int x = 0; x < side - 1; ++x) {
            const auto at = [=](int px, int py) {
                return vertices[static_cast<std::size_t>(py * side + px)];
            };
            REQUIRE(mesh.add_face({at(x, y), at(x + 1, y), at(x + 1, y + 1)}));
            REQUIRE(mesh.add_face({at(x, y), at(x + 1, y + 1), at(x, y + 1)}));
        }
    }
    return mesh;
}

void qem_is_deterministic_and_preserves_boundary() {
    const auto compiled = grid_mesh(3).compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{601U}, "grid", "concrete");
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions options;
    options.target_triangles = 6U;
    options.max_collapses = 4U;
    const auto first = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options);
    const auto second = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options);
    REQUIRE(first && second);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().triangle_count() == 6U);
    REQUIRE(!first.value().collapses.empty());
    REQUIRE(first.value().validate(compiled.value(), domain.value()));
    for (const auto& collapse : first.value().collapses) REQUIRE(collapse.validate());

    auto forged = first.value();
    forged.digest[0] = forged.digest[0] == '0' ? '1' : '0';
    REQUIRE(!forged.validate(compiled.value(), domain.value()));
}

void qem_rejects_unsafe_or_unreachable_requests() {
    const auto compiled = grid_mesh(2).compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{602U});
    REQUIRE(domain);

    carto::surface::SurfaceQemOptions invalid;
    invalid.target_triangles = 0U;
    REQUIRE(!invalid.validate());
    invalid.target_triangles = 99U;
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), invalid));

    carto::surface::SurfaceQemOptions boundary_protected;
    boundary_protected.target_triangles = 1U;
    boundary_protected.max_collapses = 8U;
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), boundary_protected));

    auto stale = domain.value();
    stale.source_revision = carto::core::Revision{777U};
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), stale, boundary_protected));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"QEM is deterministic and preserves boundary", qem_is_deterministic_and_preserves_boundary},
        {"QEM rejects unsafe or unreachable requests", qem_rejects_unsafe_or_unreachable_requests},
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
