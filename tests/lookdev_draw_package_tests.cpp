#include <carto/production/lookdev_draw_package.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view text) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

carto::production::RenderMeshVertex vertex(
    std::uint64_t id,
    carto::core::Vec3d position,
    carto::core::Vec2d uv) {
    return {
        carto::geometry::VertexId{id},
        position,
        {0.0, 0.0, 1.0},
        uv,
        std::nullopt,
        std::nullopt,
        carto::core::Vec4d{1.0, 0.0, 0.0, 1.0},
    };
}

carto::production::RenderMeshArtifact mesh_fixture() {
    carto::production::RenderMeshArtifact mesh;
    mesh.source_revision = carto::core::Revision{11U};
    mesh.mesh_revision = carto::core::Revision{7U};
    mesh.source_digest = digest("public-lookdev-mesh-source");
    mesh.vertices = {
        vertex(1U, {-0.9, -0.8, 0.0}, {0.0, 1.0}),
        vertex(2U, {-0.1, -0.8, 0.0}, {1.0, 1.0}),
        vertex(3U, {-0.5, 0.8, 0.0}, {0.5, 0.0}),
        vertex(4U, {0.1, -0.8, 0.0}, {0.0, 1.0}),
        vertex(5U, {0.9, -0.8, 0.0}, {1.0, 1.0}),
        vertex(6U, {0.5, 0.8, 0.0}, {0.5, 0.0}),
    };
    mesh.indices = {0U, 1U, 2U, 3U, 4U, 5U};
    mesh.triangle_faces = {
        carto::geometry::FaceId{101U}, carto::geometry::FaceId{102U}};
    mesh.submeshes = {
        {3U, 0U, 3U},
        {9U, 3U, 3U},
    };
    require(static_cast<bool>(mesh.validate()), "lookdev mesh fixture validates");
    return mesh;
}

carto::render::MaterialAsset material_fixture(
    carto::render::MaterialId id,
    carto::core::Revision revision,
    double red,
    double roughness) {
    carto::render::StandardMaterial definition;
    definition.base_color_factor = {red, 0.2, 0.08, 1.0};
    definition.metallic = 0.25;
    definition.roughness = roughness;
    definition.dielectric_f0 = 0.05;
    definition.clearcoat = id == 71U ? 0.6 : 0.0;
    definition.clearcoat_roughness = 0.18;
    return {id, id == 71U ? "painted-siding" : "metal-trim", definition, revision};
}

std::vector<carto::production::LookdevMaterialBinding> binding_fixture() {
    using namespace carto::production;
    const auto siding = compile_material_artifact(
        "project/public-fixture/material/71",
        material_fixture(71U, carto::core::Revision{5U}, 0.65, 0.42));
    const auto trim = compile_material_artifact(
        "project/public-fixture/material/72",
        material_fixture(72U, carto::core::Revision{3U}, 0.12, 0.2));
    require(siding && trim, "lookdev material fixtures compile");
    // Deliberately reversed: package compilation owns canonical slot order.
    return {
        {9U, trim.value()},
        {3U, siding.value()},
    };
}

void package_compilation_is_deterministic_and_native_ready() {
    using namespace carto::production;
    const auto mesh = mesh_fixture();
    const auto bindings = binding_fixture();
    const auto first = compile_lookdev_draw_package(mesh, bindings);
    auto reordered = bindings;
    std::reverse(reordered.begin(), reordered.end());
    const auto second = compile_lookdev_draw_package(mesh, reordered);
    require(first && second, "exact material set compiles for every submesh");
    require(first.value().package_digest == second.value().package_digest,
            "material binding input order cannot change package identity");
    require(first.value().draws.size() == 2U &&
                first.value().draws[0U].material_slot == 3U &&
                first.value().draws[0U].material_id == 71U &&
                first.value().draws[1U].material_slot == 9U &&
                first.value().draws[1U].material_id == 72U,
            "draws preserve canonical mesh subranges and explicit slot mapping");
    require(first.value().mesh_resident_bytes == 336U &&
                first.value().resident_bytes() ==
                    336U + 2U * sizeof(carto::render::GpuMaterialConstants),
            "lookdev package reports exact mesh and per-draw material residency");
    require(static_cast<bool>(first.value().validate()) &&
                static_cast<bool>(validate_lookdev_draw_package_sources(
                    first.value(), mesh, bindings)),
            "current exact mesh and material artifacts admit the package");
}

void package_round_trip_and_tamper_rejection() {
    using namespace carto::production;
    const auto compiled = compile_lookdev_draw_package(mesh_fixture(), binding_fixture());
    require(static_cast<bool>(compiled), "round-trip lookdev package compiles");
    const auto serialized = serialize_lookdev_draw_package(compiled.value());
    require(static_cast<bool>(serialized), "lookdev package serializes");
    const auto reopened = deserialize_lookdev_draw_package(serialized.value());
    require(reopened &&
                reopened.value().package_digest == compiled.value().package_digest &&
                reopened.value().mesh_artifact_digest ==
                    compiled.value().mesh_artifact_digest &&
                reopened.value().draws[0U].material_artifact_digest ==
                    compiled.value().draws[0U].material_artifact_digest,
            "lookdev package survives digest-verified save/reopen");

    auto tampered = serialized.value();
    tampered[tampered.size() / 2U] ^= 1U;
    require(!deserialize_lookdev_draw_package(tampered),
            "lookdev package rejects altered bindings or constants");
    auto truncated = serialized.value();
    truncated.pop_back();
    require(!deserialize_lookdev_draw_package(truncated),
            "lookdev package rejects a truncated digest");
    auto trailing = serialized.value();
    trailing.push_back(0U);
    require(!deserialize_lookdev_draw_package(trailing),
            "lookdev package rejects trailing unbound data");
    auto wrong_magic = serialized.value();
    wrong_magic.front() = 'X';
    require(!deserialize_lookdev_draw_package(wrong_magic),
            "lookdev package rejects unknown framing");

    auto mutated_range = compiled.value();
    mutated_range.draws[0U].first_index = 1U;
    require(!mutated_range.refresh_digest(),
            "non-contiguous draw ranges cannot be legitimized by redigesting");
    auto mutated_material = compiled.value();
    mutated_material.draws[0U].gpu_constants.surface_factors[1U] = 0.9F;
    require(!mutated_material.refresh_digest(),
            "material constants remain bound to their compiled artifact digest");
}

void stale_missing_and_ambiguous_sources_fail_closed() {
    using namespace carto::production;
    const auto mesh = mesh_fixture();
    const auto bindings = binding_fixture();
    const auto compiled = compile_lookdev_draw_package(mesh, bindings);
    require(static_cast<bool>(compiled), "stale-source package fixture compiles");

    auto changed_mesh = mesh;
    changed_mesh.vertices[0U].position.x = -0.95;
    require(static_cast<bool>(changed_mesh.validate()),
            "same-revision changed mesh remains structurally valid adversarial input");
    const auto stale_mesh = validate_lookdev_draw_package_sources(
        compiled.value(), changed_mesh, bindings);
    require(!stale_mesh && stale_mesh.error().code == carto::core::ErrorCode::stale_data,
            "same-revision mesh content substitution is rejected as stale");

    auto changed_bindings = bindings;
    const auto replacement = compile_material_artifact(
        "project/public-fixture/material/71",
        material_fixture(71U, carto::core::Revision{5U}, 0.65, 0.8));
    require(static_cast<bool>(replacement), "changed material fixture compiles");
    changed_bindings[1U].material = replacement.value();
    const auto stale_material = validate_lookdev_draw_package_sources(
        compiled.value(), mesh, changed_bindings);
    require(!stale_material &&
                stale_material.error().code == carto::core::ErrorCode::stale_data,
            "same-revision material content substitution is rejected as stale");

    std::vector<LookdevMaterialBinding> missing{bindings.front()};
    require(!compile_lookdev_draw_package(mesh, missing),
            "missing material-slot binding fails closed");
    auto duplicate = bindings;
    duplicate[0U].material_slot = 3U;
    require(!compile_lookdev_draw_package(mesh, duplicate),
            "duplicate material-slot binding fails closed");
    auto wrong_slot = bindings;
    wrong_slot[0U].material_slot = 17U;
    require(!compile_lookdev_draw_package(mesh, wrong_slot),
            "unknown extra slot cannot substitute for a missing mesh slot");
    auto forged = bindings;
    forged[0U].material.gpu_constants.base_color_factor[0U] = 0.99F;
    require(!compile_lookdev_draw_package(mesh, forged),
            "tampered compiled material cannot enter a lookdev package");

    auto un_slotted_mesh = mesh;
    un_slotted_mesh.submeshes = {{std::nullopt, 0U, 6U}};
    require(static_cast<bool>(un_slotted_mesh.validate()),
            "one un-slotted submesh remains a valid mesh product");
    std::vector<LookdevMaterialBinding> un_slotted{
        {std::nullopt, bindings[1U].material}};
    require(static_cast<bool>(compile_lookdev_draw_package(un_slotted_mesh, un_slotted)),
            "one explicit un-slotted material binds deterministically");
    un_slotted.front().material_slot = 0U;
    require(!compile_lookdev_draw_package(un_slotted_mesh, un_slotted),
            "implicit slot zero cannot replace the explicit un-slotted contract");
}

} // namespace

int main() {
    package_compilation_is_deterministic_and_native_ready();
    package_round_trip_and_tamper_rejection();
    stale_missing_and_ambiguous_sources_fail_closed();
    std::cout << "lookdev draw package tests passed\n";
    return EXIT_SUCCESS;
}
