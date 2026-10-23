#include <carto/assets/blob_store.hpp>
#include <carto/core/json.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/surface/core.hpp>
#include <carto/surface/fidelity_package_json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

carto::geometry::EditableMesh test_mesh() {
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

std::string digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

carto::surface::FidelityPackage make_package(
    const carto::geometry::CompiledMesh& compiled) {
    const auto domain = carto::surface::build_surface_domain(
        compiled, carto::surface::SurfaceDomainId{17U}, "wall", "concrete");
    REQUIRE(domain);
    carto::surface::SurfaceClosureCompileOptions options;
    options.target_triangles = 1U;
    const auto closure = carto::surface::compile_surface_representation(
        compiled, domain.value(), options);
    REQUIRE(closure);

    const carto::surface::RepresentationId root_id{
        closure.value().representation.value == 1U ? 2U : 1U};
    carto::surface::SurfaceRepresentationTree tree;
    REQUIRE(tree.add({
        root_id,
        domain.value().id,
        std::nullopt,
        {closure.value().representation},
        carto::surface::SurfaceRepresentationKind::authored,
        {},
        digest("surface-root"),
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

    carto::surface::RepresentationMigration migration{
        domain.value().id,
        root_id,
        closure.value().representation,
        {carto::surface::MigrationTarget::ndf,
         carto::surface::MigrationTarget::directional_visibility},
        0.0,
        compiled.source_revision,
        "surface-test-migration/1",
        {},
    };
    REQUIRE(migration.refresh_digest());

    carto::surface::FidelityPackage package;
    package.source_revision = compiled.source_revision;
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
    REQUIRE(package.validate(compiled));
    return package;
}

std::string decoded_member(const std::vector<carto::core::json::Member>& object,
                           std::string_view key) {
    const auto* member = carto::core::json::find_member(object, key);
    REQUIRE(member != nullptr);
    const auto value = carto::core::json::decode_string(member->raw_value);
    REQUIRE(value);
    return value.value();
}

void emits_cartographer_contract_and_canonical_digest() {
    const auto compiled = test_mesh().compile();
    REQUIRE(compiled);
    const auto package = make_package(compiled.value());
    const auto encoded = carto::surface::serialize_fidelity_package_json(
        package, compiled.value());
    REQUIRE(encoded);

    const auto object = carto::core::json::parse_object(encoded.value());
    REQUIRE(object);
    REQUIRE(decoded_member(object.value(), "kind") == "cartographer_fidelity_package");
    const auto* format = carto::core::json::find_member(object.value(), "format_version");
    REQUIRE(format != nullptr && format->raw_value == "2");
    const auto* domains = carto::core::json::find_member(object.value(), "domains");
    const auto* representations = carto::core::json::find_member(object.value(), "representations");
    const auto* runtime = carto::core::json::find_member(object.value(), "runtime_records");
    const auto* closures = carto::core::json::find_member(object.value(), "closures");
    const auto* migrations = carto::core::json::find_member(object.value(), "migrations");
    REQUIRE(domains != nullptr && domains->raw_value.front() == '[');
    REQUIRE(representations != nullptr && representations->raw_value.front() == '[');
    REQUIRE(runtime != nullptr && runtime->raw_value.front() == '[');
    REQUIRE(closures != nullptr && closures->raw_value.front() == '[');
    REQUIRE(migrations != nullptr && migrations->raw_value.front() == '[');

    const std::string package_digest = decoded_member(object.value(), "package_digest");
    REQUIRE(package_digest.size() == 64U);
    REQUIRE(carto::assets::Sha256Digest::from_hex(package_digest));
    REQUIRE(package_digest != package.digest);
}

void ordering_is_stable_and_source_is_fail_closed() {
    auto compiled = test_mesh().compile();
    REQUIRE(compiled);
    auto package = make_package(compiled.value());
    const auto first = carto::surface::serialize_fidelity_package_json(
        package, compiled.value());
    REQUIRE(first);

    std::reverse(package.representations.begin(), package.representations.end());
    REQUIRE(package.refresh_digest());
    const auto reordered = carto::surface::serialize_fidelity_package_json(
        package, compiled.value());
    REQUIRE(reordered);
    REQUIRE(first.value() == reordered.value());

    auto stale_source = compiled.value();
    stale_source.positions.front().x += 0.25;
    const auto stale = carto::surface::serialize_fidelity_package_json(
        package, stale_source);
    REQUIRE(!stale);

    auto tampered = package;
    tampered.digest[0] = tampered.digest[0] == '0' ? '1' : '0';
    const auto invalid = carto::surface::serialize_fidelity_package_json(
        tampered, compiled.value());
    REQUIRE(!invalid);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"emits neutral contract and canonical digest", emits_cartographer_contract_and_canonical_digest},
        {"ordering is stable and source is fail closed", ordering_is_stable_and_source_is_fail_closed},
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
