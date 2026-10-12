#include <carto/physical/construction.hpp>
#include <carto/physical/geometry_evidence.hpp>
#include <carto/physical/mass.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1e-9) {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= epsilon * scale;
}

// Hand-built 2x2x2 meter box centered on the origin, outward wound, with
// stable authoring correspondence ids. Volume 8 m^3, surface area 24 m^2,
// every face one meter from the bounds center.
carto::geometry::CompiledMesh box_mesh() {
    carto::geometry::CompiledMesh mesh;
    mesh.positions = {
        {-1.0, -1.0, -1.0}, {1.0, -1.0, -1.0}, {1.0, 1.0, -1.0}, {-1.0, 1.0, -1.0},
        {-1.0, -1.0, 1.0},  {1.0, -1.0, 1.0},  {1.0, 1.0, 1.0},  {-1.0, 1.0, 1.0},
    };
    mesh.normals = {
        {-1.0, -1.0, -1.0}, {1.0, -1.0, -1.0}, {1.0, 1.0, -1.0}, {-1.0, 1.0, -1.0},
        {-1.0, -1.0, 1.0},  {1.0, -1.0, 1.0},  {1.0, 1.0, 1.0},  {-1.0, 1.0, 1.0},
    };
    for (auto& normal : mesh.normals) normal = normal.normalized();
    mesh.indices = {
        0, 2, 1, 0, 3, 2, // -z
        4, 5, 6, 4, 6, 7, // +z
        0, 7, 3, 0, 4, 7, // -x
        1, 6, 5, 1, 2, 6, // +x
        0, 1, 5, 0, 5, 4, // -y
        3, 7, 6, 3, 6, 2, // +y
    };
    mesh.vertex_ids = {
        carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U},
        carto::geometry::VertexId{3U}, carto::geometry::VertexId{4U},
        carto::geometry::VertexId{5U}, carto::geometry::VertexId{6U},
        carto::geometry::VertexId{7U}, carto::geometry::VertexId{8U},
    };
    for (std::uint64_t face = 1U; face <= 12U; ++face) {
        mesh.triangle_faces.push_back(carto::geometry::FaceId{face});
        mesh.triangle_edges.push_back(
            {std::nullopt, std::nullopt, std::nullopt});
    }
    mesh.bounds = carto::core::Bounds3d{{-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}};
    mesh.source_revision = carto::core::Revision{1U};
    return mesh;
}

carto::physical::PhysicalMaterialIntent intent(
    std::string identity,
    double density) {
    carto::physical::PhysicalMaterialIntent value;
    value.catalog_identity = std::move(identity);
    value.catalog_revision = carto::core::Revision{3U};
    value.density_kg_per_m3 = density;
    return value;
}

void evidence_matches_hand_computed_box_values() {
    const auto evidence =
        carto::physical::derive_geometry_evidence(77U, box_mesh());
    require(static_cast<bool>(evidence), "box geometry evidence derives");
    require(near(evidence.value().volume, 8.0), "box volume is 8 m^3");
    require(near(evidence.value().signed_volume, 8.0), "box signed volume is 8 m^3");
    require(near(evidence.value().surface_area, 24.0), "box surface area is 24 m^2");
    require(evidence.value().winding_consistent, "box winding is consistent");
    require(evidence.value().triangle_count == 12U, "box has twelve triangles");
    require(near(evidence.value().volume_centroid.x, 0.0) &&
            near(evidence.value().volume_centroid.y, 0.0) &&
            near(evidence.value().volume_centroid.z, 0.0),
        "box volume centroid is the origin");
    // Unit-density box: I_axis = V * (b^2 + c^2) / 12 = 8 * 8 / 12 = 16/3.
    require(near(evidence.value().unit_density_inertia.xx, 16.0 / 3.0) &&
            near(evidence.value().unit_density_inertia.yy, 16.0 / 3.0) &&
            near(evidence.value().unit_density_inertia.zz, 16.0 / 3.0),
        "box unit inertia diagonal is 16/3");
    require(near(evidence.value().unit_density_inertia.xy, 0.0) &&
            near(evidence.value().unit_density_inertia.xz, 0.0) &&
            near(evidence.value().unit_density_inertia.yz, 0.0),
        "box unit inertia off-diagonal is zero");
    require(near(evidence.value().thickness.minimum, 1.0) &&
            near(evidence.value().thickness.maximum, 1.0) &&
            near(evidence.value().thickness.mean, 1.0),
        "box thickness distribution is exactly one meter");
    require(evidence.value().thickness.samples == 12U && !evidence.value().thickness.ambiguous,
        "box thickness sampled per triangle without ambiguity");

    const auto decoded = carto::physical::GeometryEvidence::deserialize(
        evidence.value().serialize());
    require(static_cast<bool>(decoded), "geometry evidence round-trips");
    require(decoded.value().canonical_digest().value() ==
            evidence.value().canonical_digest().value(),
        "geometry evidence digest is stable across serialization");
}

void mass_derivation_matches_hand_computed_values() {
    const auto evidence = carto::physical::derive_geometry_evidence(77U, box_mesh());
    require(static_cast<bool>(evidence), "box geometry evidence derives");

    carto::physical::ConstructionProfile solid;
    solid.kind = carto::physical::ConstructionKind::solid;
    solid.source_revision = carto::core::Revision{1U};
    solid.primary_material = intent("vanta.physical.materials/test-uniform", 1000.0);

    const auto mass = carto::physical::derive_mass(evidence.value(), solid);
    require(static_cast<bool>(mass), "solid box mass derives");
    require(near(mass.value().total_mass_kg, 8000.0), "solid box mass is 8000 kg");
    require(near(mass.value().total_volume_m3, 8.0), "derived volume is 8 m^3");
    require(near(mass.value().center_of_mass.x, 0.0) &&
            near(mass.value().center_of_mass.y, 0.0) &&
            near(mass.value().center_of_mass.z, 0.0),
        "solid box center of mass is the origin");
    // I_axis = m * (b^2 + c^2) / 12 = 8000 * 8 / 12.
    require(near(mass.value().inertia_kg_m2.xx, 8000.0 * 8.0 / 12.0) &&
            near(mass.value().inertia_kg_m2.yy, 8000.0 * 8.0 / 12.0) &&
            near(mass.value().inertia_kg_m2.zz, 8000.0 * 8.0 / 12.0),
        "solid box inertia diagonal matches the closed form");
    require(near(mass.value().inertia_kg_m2.xy, 0.0) &&
            near(mass.value().inertia_kg_m2.xz, 0.0) &&
            near(mass.value().inertia_kg_m2.yz, 0.0),
        "solid box inertia off-diagonal is zero");
    require(mass.value().geometry_evidence_digest ==
                evidence.value().canonical_digest().value(),
        "mass receipt binds the exact geometry evidence digest");
    require(mass.value().construction_profile_digest == solid.canonical_digest().value(),
        "mass receipt binds the exact construction digest");

    // Shell: 24 m^2 * 0.1 m wall at 1000 kg/m^3 = 2400 kg.
    carto::physical::ConstructionProfile shell;
    shell.kind = carto::physical::ConstructionKind::shell;
    shell.source_revision = carto::core::Revision{1U};
    shell.primary_material = intent("vanta.physical.materials/test-wall", 1000.0);
    shell.wall_thickness = 0.1;
    shell.wall_thickness_source =
        carto::physical::ThicknessSource::derived_from_geometry;
    const auto shell_mass = carto::physical::derive_mass(evidence.value(), shell);
    require(static_cast<bool>(shell_mass), "shell box mass derives");
    require(near(shell_mass.value().total_mass_kg, 2400.0), "shell box mass is 2400 kg");

    // Filled: 0.1 m wall at 2000 kg/m^3 (24 * 0.1 * 2000 = 4800 kg) around
    // 5.6 m^3 of fill at 500 kg/m^3 (2800 kg).
    carto::physical::ConstructionProfile filled;
    filled.kind = carto::physical::ConstructionKind::filled;
    filled.source_revision = carto::core::Revision{1U};
    filled.primary_material = intent("vanta.physical.materials/test-wall", 2000.0);
    filled.wall_thickness = 0.1;
    filled.wall_thickness_source = carto::physical::ThicknessSource::authored;
    filled.fill_material = intent("vanta.physical.materials/test-fill", 500.0);
    const auto filled_mass = carto::physical::derive_mass(evidence.value(), filled);
    require(static_cast<bool>(filled_mass), "filled box mass derives");
    require(near(filled_mass.value().total_mass_kg, 4800.0 + 5.6 * 500.0),
        "filled box mass combines wall and fill");
}

void layered_mass_binds_stratigraphy() {
    const auto evidence = carto::physical::derive_geometry_evidence(77U, box_mesh());
    require(static_cast<bool>(evidence), "box geometry evidence derives");

    carto::surface::MaterialStratigraphy stratigraphy;
    stratigraphy.domain = carto::surface::SurfaceDomainId{9U};
    stratigraphy.source_revision = carto::core::Revision{1U};
    stratigraphy.layers = {
        {42U, 0.05, 0.0, "paint"},
        {43U, 0.10, 0.0, "plaster"},
    };
    require(static_cast<bool>(stratigraphy.refresh_digest()),
        "stratigraphy digest refreshes");

    carto::physical::ConstructionProfile layered;
    layered.kind = carto::physical::ConstructionKind::layered;
    layered.source_revision = carto::core::Revision{1U};
    layered.primary_material = intent("vanta.physical.materials/test-substrate", 900.0);
    layered.stratigraphy = stratigraphy;
    layered.layers = {
        {0.05, intent("vanta.physical.materials/test-paint", 1000.0)},
        {0.10, intent("vanta.physical.materials/test-plaster", 500.0)},
    };
    require(static_cast<bool>(layered.validate()), "layered profile validates");

    const auto mass = carto::physical::derive_mass(evidence.value(), layered);
    require(static_cast<bool>(mass), "layered box mass derives");
    // 24 m^2 * (0.05 m * 1000 + 0.10 m * 500) kg/m^2 = 24 * 100 = 2400 kg.
    require(near(mass.value().total_mass_kg, 2400.0), "layered box mass sums layers");

    carto::physical::ConstructionProfile drifted = layered;
    drifted.layers[1].nominal_thickness = 0.11;
    require(!drifted.validate(), "layered profile rejects stratigraphy drift");

    carto::physical::ConstructionProfile missing_density = layered;
    missing_density.layers[1].material.density_kg_per_m3 = std::nullopt;
    require(!carto::physical::derive_mass(evidence.value(), missing_density),
        "mass derivation fails closed without an authored density");
}

void derivation_is_deterministic() {
    const auto first = carto::physical::derive_geometry_evidence(77U, box_mesh());
    const auto second = carto::physical::derive_geometry_evidence(77U, box_mesh());
    require(first.value().serialize() == second.value().serialize(),
        "re-derived geometry evidence is byte-identical");
    require(first.value().canonical_digest().value() ==
            second.value().canonical_digest().value(),
        "re-derived geometry evidence digests match");

    carto::physical::ConstructionProfile solid;
    solid.kind = carto::physical::ConstructionKind::solid;
    solid.source_revision = carto::core::Revision{1U};
    solid.primary_material = intent("vanta.physical.materials/test-uniform", 1000.0);
    const auto first_mass =
        carto::physical::derive_mass(first.value(), solid);
    const auto second_mass =
        carto::physical::derive_mass(second.value(), solid);
    require(first_mass.value().serialize() == second_mass.value().serialize(),
        "re-derived mass receipts are byte-identical");
    require(first_mass.value().canonical_digest().value() ==
            second_mass.value().canonical_digest().value(),
        "re-derived mass receipt digests match");
    require(carto::physical::MassDerivationReceipt::deserialize(
                first_mass.value().serialize())
                .value()
                .canonical_digest()
                .value() == first_mass.value().canonical_digest().value(),
        "mass receipt digest survives a serialization round trip");
}

void hostile_inputs_fail_closed() {
    require(!carto::physical::GeometryEvidence::deserialize(""),
        "empty evidence text is rejected");
    require(!carto::physical::derive_geometry_evidence(0U, box_mesh()),
        "zero mesh identity is rejected");
    require(!carto::physical::derive_geometry_evidence(77U, carto::geometry::CompiledMesh{}),
        "invalid compiled mesh is rejected");

    // Invert winding: the signed volume integral goes negative and the
    // derivation refuses rather than silently absolute-valuing it.
    carto::geometry::CompiledMesh flipped = box_mesh();
    for (std::size_t triangle = 0U; triangle < flipped.indices.size() / 3U; ++triangle) {
        std::swap(flipped.indices[triangle * 3U], flipped.indices[triangle * 3U + 1U]);
    }
    require(!carto::physical::derive_geometry_evidence(77U, flipped),
        "inconsistently wound mesh is rejected");

    const auto evidence = carto::physical::derive_geometry_evidence(77U, box_mesh());
    const std::string encoded = evidence.value().serialize();
    require(!carto::physical::GeometryEvidence::deserialize(encoded + "trailing"),
        "evidence parser rejects trailing unbound data");
    std::string future = encoded;
    future.replace(0U,
        std::string{"CARTOGRAPHER_PHYSICAL_GEOMETRY_EVIDENCE 1"}.size(),
        "CARTOGRAPHER_PHYSICAL_GEOMETRY_EVIDENCE 99");
    const auto rejected = carto::physical::GeometryEvidence::deserialize(future);
    require(!rejected && rejected.error().code == carto::core::ErrorCode::version_mismatch,
        "unknown future evidence versions fail with version_mismatch");

    std::string tampered = encoded;
    require(!carto::physical::GeometryEvidence::deserialize(
                tampered.replace(tampered.find("VOLUME"), 6U, "VOLIME")),
        "malformed evidence records are rejected");
}

} // namespace

int main() {
    evidence_matches_hand_computed_box_values();
    mass_derivation_matches_hand_computed_values();
    layered_mass_binds_stratigraphy();
    derivation_is_deterministic();
    hostile_inputs_fail_closed();
    std::cout << "physical mass tests passed\n";
    return EXIT_SUCCESS;
}
