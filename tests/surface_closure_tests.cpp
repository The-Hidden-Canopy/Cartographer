#include <carto/assets/blob_store.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/surface/core.hpp>
#include <carto/surface/process.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
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

carto::geometry::EditableMesh two_surface_patch() {
    carto::geometry::EditableMesh mesh;
    const auto a0 = mesh.add_vertex({-1.0, 0.0, 0.0});
    const auto a1 = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto a2 = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto b0 = mesh.add_vertex({-1.0, 0.0, 1.0});
    const auto b1 = mesh.add_vertex({1.0, 0.0, 1.0});
    const auto b2 = mesh.add_vertex({0.0, 1.0, 2.0});
    REQUIRE(a0 && a1 && a2 && b0 && b1 && b2);
    REQUIRE(mesh.add_face({a0.value(), a1.value(), a2.value()}));
    REQUIRE(mesh.add_face({b0.value(), b1.value(), b2.value()}));
    return mesh;
}

std::string test_digest(std::string value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

void domain_and_analysis_are_revision_bound() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{17U}, "wall", "concrete");
    REQUIRE(domain);
    REQUIRE(domain.value().validate(compiled.value()));

    const auto analysis = carto::surface::analyze_surface(compiled.value(), domain.value());
    REQUIRE(analysis);
    REQUIRE(analysis.value().validate(compiled.value()));
    REQUIRE(analysis.value().patch.area > 0.0);
    REQUIRE(analysis.value().ndf.alpha_x >= 0.001);
    REQUIRE(analysis.value().ndf.alpha_y >= 0.001);
    REQUIRE(analysis.value().visibility.ambient_visibility >= 0.0);
    REQUIRE(analysis.value().visibility.ambient_visibility <= 1.0);
    REQUIRE(analysis.value().edges.total() == 6U);

    auto stale = domain.value();
    stale.source_revision = carto::core::Revision{999U};
    REQUIRE(!stale.validate(compiled.value()));
    auto forged = domain.value();
    forged.digest.assign(64U, '0');
    REQUIRE(!forged.validate(compiled.value()));

    auto same_revision_geometry = compiled.value();
    same_revision_geometry.positions.front().x += 0.25;
    REQUIRE(!domain.value().validate(same_revision_geometry));
}

void frequency_and_proxy_are_deterministic_and_bounded() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{18U});
    REQUIRE(domain);
    const auto first = carto::surface::decompose_frequency(compiled.value(), domain.value());
    const auto second = carto::surface::decompose_frequency(compiled.value(), domain.value());
    REQUIRE(first && second);
    REQUIRE(first.value().size() == 4U);
    REQUIRE(first.value().size() == second.value().size());
    for (std::size_t index = 0U; index < first.value().size(); ++index) {
        REQUIRE(first.value()[index].validate());
        REQUIRE(first.value()[index].triangle_count == second.value()[index].triangle_count);
        REQUIRE(first.value()[index].rms_normal_change == second.value()[index].rms_normal_change);
    }

    const auto proxy = carto::surface::build_geometry_proxy(compiled.value(), 1U);
    REQUIRE(proxy);
    REQUIRE(proxy.value().triangle_count() == 1U);
    REQUIRE(proxy.value().validate(compiled.value()));
    auto invalid = carto::surface::build_geometry_proxy(compiled.value(), 0U);
    REQUIRE(!invalid);
    invalid = carto::surface::build_geometry_proxy(compiled.value(), 3U);
    REQUIRE(!invalid);
}

void closure_beats_geometry_only_in_bounded_reference_space() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{19U}, "patch", "rough");
    REQUIRE(domain);
    carto::surface::SurfaceClosureCompileOptions options;
    options.target_triangles = 1U;
    options.analysis.visibility_sample_count = 32U;
    options.evaluation.view_count = 8U;
    options.evaluation.light_count = 8U;
    const auto closure = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options, carto::surface::RepresentationMode::fidelity_closure);
    const auto baseline = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options, carto::surface::RepresentationMode::geometry_only);
    REQUIRE(closure && baseline);
    REQUIRE(closure.value().validate(compiled.value(), domain.value()));
    REQUIRE(baseline.value().validate(compiled.value(), domain.value()));
    REQUIRE(closure.value().error.rms_radiance_error <= baseline.value().error.rms_radiance_error);
    REQUIRE(closure.value().error.specular_error <= baseline.value().error.specular_error);

    carto::surface::SurfaceQualityPolicy permissive;
    permissive.max_rms_radiance_error = 10.0;
    permissive.max_radiance_error = 10.0;
    permissive.max_diffuse_error = 10.0;
    permissive.max_specular_error = 10.0;
    permissive.max_visibility_error = 10.0;
    permissive.max_silhouette_error = 10.0;
    REQUIRE(carto::surface::admit_surface_representation(
        compiled.value(), domain.value(), closure.value(), permissive));

    auto rejected = closure.value();
    rejected.error.max_radiance_error = 100.0;
    REQUIRE(!carto::surface::admit_surface_representation(
        compiled.value(), domain.value(), rejected));
}

void representation_tree_and_vanta_package_are_fail_closed() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{20U});
    REQUIRE(domain);
    carto::surface::SurfaceClosureCompileOptions options;
    options.target_triangles = 1U;
    const auto closure = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options);
    REQUIRE(closure);

    const auto invalid_mode = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options,
        static_cast<carto::surface::RepresentationMode>(99));
    REQUIRE(!invalid_mode);

    auto invalid_closure = closure.value();
    invalid_closure.kind = static_cast<carto::surface::SurfaceRepresentationKind>(99);
    REQUIRE(!invalid_closure.refresh_digest());
    REQUIRE(!invalid_closure.validate(compiled.value(), domain.value()));

    const carto::surface::RepresentationId root_id{
        closure.value().representation.value == 1U ? 2U : 1U};
    const std::string root_digest = test_digest("surface-root");
    carto::surface::SurfaceRepresentationTree tree;
    REQUIRE(tree.add({
        root_id,
        domain.value().id,
        std::nullopt,
        {closure.value().representation},
        carto::surface::SurfaceRepresentationKind::authored,
        {},
        root_digest,
    }));
    REQUIRE(tree.add({
        closure.value().representation,
        domain.value().id,
        root_id,
        {},
        carto::surface::SurfaceRepresentationKind::closure_proxy,
        closure.value().error,
        closure.value().digest,
    }));
    REQUIRE(tree.validate(domain.value().id));

    auto invalid_node = tree.nodes().front();
    invalid_node.kind = static_cast<carto::surface::SurfaceRepresentationKind>(99);
    REQUIRE(!invalid_node.validate());

    carto::surface::RepresentationMigration migration{
        domain.value().id,
        root_id,
        closure.value().representation,
        {carto::surface::MigrationTarget::ndf,
         carto::surface::MigrationTarget::directional_visibility},
        0.0,
        compiled.value().source_revision,
        "surface-test-migration/1",
        {},
    };
    REQUIRE(migration.refresh_digest());
    REQUIRE(migration.validate());

    auto invalid_migration = migration;
    invalid_migration.targets.front() = static_cast<carto::surface::MigrationTarget>(99);
    REQUIRE(!invalid_migration.refresh_digest());
    REQUIRE(!invalid_migration.validate());

    carto::surface::VantaFidelityPackage package;
    package.source_revision = compiled.value().source_revision;
    package.domains.push_back(domain.value());
    package.representations = tree.nodes();
    package.runtime_records.push_back({
        domain.value().id,
        closure.value().representation,
        closure.value().error.max_position_error,
        closure.value().error.rms_radiance_error,
        1U,
        2U,
        3U,
    });
    package.closures.push_back(closure.value());
    package.migrations.push_back(migration);
    REQUIRE(package.refresh_digest());
    REQUIRE(package.validate(compiled.value()));

    const auto foreign_domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{21U});
    REQUIRE(foreign_domain);
    const carto::surface::RepresentationId foreign_root_id{
        closure.value().representation.value == 999U ? 1000U : 999U};
    const carto::surface::SurfaceRepresentationNode foreign_root{
        foreign_root_id,
        foreign_domain.value().id,
        std::nullopt,
        {},
        carto::surface::SurfaceRepresentationKind::authored,
        {},
        test_digest("foreign-root"),
    };
    REQUIRE(foreign_root.validate());

    auto cross_domain_base = package;
    cross_domain_base.domains.push_back(foreign_domain.value());
    cross_domain_base.representations.push_back(foreign_root);
    REQUIRE(cross_domain_base.refresh_digest());
    REQUIRE(cross_domain_base.validate(compiled.value()));

    auto cross_domain_runtime = cross_domain_base;
    cross_domain_runtime.runtime_records.front().representation = foreign_root_id;
    REQUIRE(cross_domain_runtime.refresh_digest());
    REQUIRE(!cross_domain_runtime.validate(compiled.value()));

    auto cross_domain_closure = cross_domain_base;
    cross_domain_closure.closures.front().representation = foreign_root_id;
    REQUIRE(cross_domain_closure.closures.front().refresh_digest());
    REQUIRE(cross_domain_closure.refresh_digest());
    REQUIRE(!cross_domain_closure.validate(compiled.value()));

    auto cross_domain_migration = cross_domain_base;
    cross_domain_migration.migrations.front().source_representation = foreign_root_id;
    REQUIRE(cross_domain_migration.migrations.front().refresh_digest());
    REQUIRE(cross_domain_migration.refresh_digest());
    REQUIRE(!cross_domain_migration.validate(compiled.value()));

    auto tampered = package;
    tampered.digest[0] = tampered.digest[0] == '0' ? '1' : '0';
    REQUIRE(!tampered.validate(compiled.value()));
    auto invalid_tree = tree;
    (void)invalid_tree;
}

void process_contracts_are_bounded_and_deterministic() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{21U}, "wall", "coated-steel");
    REQUIRE(domain);
    const auto analysis = carto::surface::analyze_surface(compiled.value(), domain.value());
    REQUIRE(analysis);

    carto::surface::MaterialStratigraphy stratigraphy{
        domain.value().id,
        compiled.value().source_revision,
        {
            {11U, 0.10, 0.01, "paint"},
            {12U, 0.20, 0.02, "primer"},
            {13U, 1.00, 0.00, "substrate"},
        },
        {},
    };
    REQUIRE(stratigraphy.refresh_digest());
    REQUIRE(stratigraphy.validate());
    const auto shallow = carto::surface::resolve_material_exposure(stratigraphy, 0.05);
    REQUIRE(shallow && shallow.value().material == 11U && !shallow.value().reaches_substrate);
    const auto primer = carto::surface::resolve_material_exposure(stratigraphy, 0.15);
    REQUIRE(primer && primer.value().material == 12U);
    const auto substrate = carto::surface::resolve_material_exposure(stratigraphy, 2.0);
    REQUIRE(substrate && substrate.value().material == 13U && substrate.value().reaches_substrate);

    carto::surface::WeatheringControls controls;
    controls.age = 40.0;
    controls.rain_exposure = 0.8;
    controls.solar_exposure = 0.7;
    controls.moisture = 0.6;
    controls.traffic = 0.4;
    controls.maintenance = 0.1;
    controls.seed = 7U;
    const auto weathering_a = carto::surface::derive_weathering(analysis.value(), controls);
    const auto weathering_b = carto::surface::derive_weathering(analysis.value(), controls);
    REQUIRE(weathering_a && weathering_b);
    REQUIRE(weathering_a.value().validate());
    REQUIRE(weathering_a.value().digest == weathering_b.value().digest);
    REQUIRE(weathering_a.value().oxidation > 0.0);

    carto::surface::ManufacturingHistory history{
        domain.value().id,
        compiled.value().source_revision,
        {
            {1U, carto::surface::ManufacturingProcess::cut, {1.0, 0.0, 0.0}, 0.8, "cut-plan"},
            {2U, carto::surface::ManufacturingProcess::grind, {1.0, 0.0, 0.0}, 0.4, "finish"},
        },
        {},
    };
    REQUIRE(history.refresh_digest());
    REQUIRE(history.validate());
    auto duplicate_history = history;
    duplicate_history.steps[1U].sequence = duplicate_history.steps[0U].sequence;
    REQUIRE(!duplicate_history.refresh_digest());

    carto::surface::SurfaceContact contact{
        carto::surface::SurfaceDomainId{21U},
        carto::surface::SurfaceDomainId{22U},
        carto::surface::ContactType::sealed,
        0.01,
        {},
    };
    REQUIRE(contact.refresh_digest());
    REQUIRE(contact.validate());
    carto::surface::ContactGraph contacts{
        compiled.value().source_revision,
        {contact},
        {},
    };
    REQUIRE(contacts.refresh_digest());
    REQUIRE(contacts.validate());

    carto::surface::EdgeProfile profile{
        carto::geometry::EdgeId{1U},
        carto::surface::EdgeProcess::painted_corner,
        carto::surface::ProfileShape::asymmetric,
        0.01,
        0.002,
        0.2,
        0.1,
        "paint-to-primer",
    };
    REQUIRE(profile.validate());

    carto::surface::MaterialGeometryPolicy policy{
        12U, 0.002, 0.001, 128U, 9U, "brick-generator/1"};
    REQUIRE(policy.validate());
    const auto intent = carto::surface::make_material_geometry_intent(
        domain.value().id, domain.value().source_faces.front(), compiled.value().source_revision, policy);
    REQUIRE(intent && intent.value().validate());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"surface domain and analysis are revision bound", domain_and_analysis_are_revision_bound},
        {"frequency and proxy are deterministic", frequency_and_proxy_are_deterministic_and_bounded},
        {"closure beats geometry-only reference", closure_beats_geometry_only_in_bounded_reference_space},
        {"representation tree and VANTA package are bounded",
         representation_tree_and_vanta_package_are_fail_closed},
        {"material, process, contact, and edge contracts are bounded",
         process_contracts_are_bounded_and_deterministic},
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
