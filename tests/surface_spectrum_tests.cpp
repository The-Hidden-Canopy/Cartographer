#include <carto/geometry/mesh.hpp>
#include <carto/surface/spectrum.hpp>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
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

carto::geometry::EditableMesh connected_square() {
    carto::geometry::EditableMesh mesh;
    const auto v0 = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto v1 = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto v2 = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto v3 = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(v0 && v1 && v2 && v3);
    REQUIRE(mesh.add_face({v0.value(), v1.value(), v2.value()}));
    REQUIRE(mesh.add_face({v0.value(), v2.value(), v3.value()}));
    return mesh;
}

void spectrum_is_deterministic_and_revision_bound() {
    const auto compiled = connected_square().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{401U}, "panel", "concrete");
    REQUIRE(domain);

    const std::vector<double> signal{0.0, 1.0, 0.0, 1.0};
    carto::surface::SurfaceSpectrumOptions options;
    options.max_vertices = 16U;
    options.max_modes = 4U;
    options.max_jacobi_sweeps = 128U;
    const auto first = carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), signal, options);
    const auto second = carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), signal, options);
    REQUIRE(first && second);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().validate(compiled.value(), domain.value()));
    REQUIRE(first.value().vertex_ids.size() == 4U);
    REQUIRE(first.value().modes.size() == 4U);
    REQUIRE(first.value().modes.front().eigenvalue <= 1e-7);
    REQUIRE(first.value().modes[1U].eigenvalue >= first.value().modes.front().eigenvalue);
    REQUIRE(first.value().total_signal_energy > 0.0);
    REQUIRE(first.value().smoothness_energy > 0.0);

    auto stale_domain = domain.value();
    stale_domain.source_revision = carto::core::Revision{999U};
    REQUIRE(!carto::surface::build_surface_spectrum(
        compiled.value(), stale_domain, signal, options));

    auto forged = first.value();
    forged.digest[0] = forged.digest[0] == '0' ? '1' : '0';
    REQUIRE(!forged.validate(compiled.value(), domain.value()));
}

void spectrum_rejects_unbounded_or_misaligned_inputs() {
    const auto compiled = connected_square().compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{402U});
    REQUIRE(domain);
    const std::vector<double> signal{0.0, 1.0, 0.0, 1.0};

    REQUIRE(!carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), std::vector<double>{0.0}, {}));
    auto non_finite = signal;
    non_finite[2] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), non_finite, {}));

    carto::surface::SurfaceSpectrumOptions too_small;
    too_small.max_vertices = 3U;
    REQUIRE(!carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), signal, too_small));

    auto invalid_options = too_small;
    invalid_options.max_vertices = 0U;
    REQUIRE(!invalid_options.validate());
    invalid_options.max_vertices = 16U;
    invalid_options.max_modes = 0U;
    REQUIRE(!invalid_options.validate());
    invalid_options.max_modes = 16U;
    invalid_options.max_jacobi_sweeps = 0U;
    REQUIRE(!invalid_options.validate());

    const auto spectrum = carto::surface::build_surface_spectrum(
        compiled.value(), domain.value(), signal, {});
    REQUIRE(spectrum);
    auto oversized_receipt = spectrum.value();
    oversized_receipt.modes.resize(5U, oversized_receipt.modes.front());
    REQUIRE(!oversized_receipt.refresh_digest());
    auto tampered_mode = spectrum.value();
    tampered_mode.modes.front().coefficients.front() += 1.0;
    REQUIRE(!tampered_mode.validate(compiled.value(), domain.value()));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"spectrum is deterministic and revision bound", spectrum_is_deterministic_and_revision_bound},
        {"spectrum rejects unbounded or misaligned inputs", spectrum_rejects_unbounded_or_misaligned_inputs},
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
