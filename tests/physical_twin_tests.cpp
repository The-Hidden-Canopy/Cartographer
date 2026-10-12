#include <carto/physical/condition.hpp>
#include <carto/physical/construction.hpp>
#include <carto/physical/geometry_evidence.hpp>
#include <carto/physical/ground_pressure.hpp>
#include <carto/physical/mass.hpp>
#include <carto/physical/twin.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

// Same outward-wound 2x2x2 m box fixture as the mass tests.
carto::geometry::CompiledMesh box_mesh() {
    carto::geometry::CompiledMesh mesh;
    mesh.positions = {
        {-1.0, -1.0, -1.0}, {1.0, -1.0, -1.0}, {1.0, 1.0, -1.0}, {-1.0, 1.0, -1.0},
        {-1.0, -1.0, 1.0},  {1.0, -1.0, 1.0},  {1.0, 1.0, 1.0},  {-1.0, 1.0, 1.0},
    };
    mesh.normals = mesh.positions;
    for (auto& normal : mesh.normals) normal = normal.normalized();
    mesh.indices = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 7, 3, 0, 4, 7, 1, 6, 5, 1, 2, 6,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
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

carto::physical::PhysicalTwinRecord twin(double density) {
    carto::physical::PhysicalTwinRecord record;
    record.source_namespace = "condition/motel-8/wall-segment";
    record.source_revision = carto::core::Revision{1U};
    record.source_digest = digest("source-blob-v1");

    const auto evidence =
        carto::physical::derive_geometry_evidence(77U, box_mesh());
    require(static_cast<bool>(evidence), "twin fixture geometry derives");
    record.geometry = evidence.value();

    carto::physical::ConstructionProfile construction;
    construction.kind = carto::physical::ConstructionKind::solid;
    construction.source_revision = carto::core::Revision{1U};
    construction.primary_material =
        intent("vanta.physical.materials/test-uniform", density);
    record.construction = construction;

    const auto mass =
        carto::physical::derive_mass(record.geometry, record.construction);
    require(static_cast<bool>(mass), "twin fixture mass derives");
    record.mass = mass.value();

    carto::physical::PhysicalSurfaceLedger ledger;
    require(static_cast<bool>(ledger.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{5U},
                carto::core::Revision{1U},
                std::uint32_t{3U},
                std::string{"vanta.physics.surfaces/concrete-smooth"},
                std::uint32_t{11U},
                std::string{"vanta.audio.surfaces/concrete"},
                std::nullopt})),
        "twin fixture ledger entry inserts");
    record.ledger = ledger;

    const auto pressure = carto::physical::derive_ground_pressure(
        record.mass, carto::core::Bounds3d{{-1.0, -1.0, -1.0}, {1.0, 0.0, 1.0}});
    require(static_cast<bool>(pressure), "twin fixture ground pressure derives");
    require(near(pressure.value().mean_pressure_pa,
                 record.mass.total_mass_kg *
                     carto::physical::kStandardGravityMetersPerSecond2 / 4.0),
        "fixture ground pressure is mass over the patch area");
    record.ground_pressure = pressure.value();

    record.condition_states = {
        carto::physical::SurfaceConditionState{
            carto::surface::SurfaceDomainId{5U},
            carto::core::Revision{1U},
            "condition",
            4242U,
            0.35,
            0.02,
            0.6,
            12.0,
        },
    };
    require(static_cast<bool>(record.validate()), "twin fixture validates");
    return record;
}

void game_owned_condition_state_is_fenced_from_the_digest() {
    static_assert(carto::physical::SurfaceConditionState::kGameOwned,
        "condition state must be compile-time marked game-owned");

    auto record = twin(1000.0);
    const auto digest_before = record.canonical_digest();
    require(static_cast<bool>(digest_before), "twin digest computes");

    // Mutating runtime-owned wear must not move the authoring digest.
    record.condition_states.front().wear = 0.99;
    record.condition_states.front().damage_depth = 0.5;
    record.condition_states.push_back(carto::physical::SurfaceConditionState{
        carto::surface::SurfaceDomainId{6U},
        carto::core::Revision{1U},
        "condition",
        5151U,
        0.1,
        0.0,
        0.2,
        3.0,
    });
    require(record.canonical_digest().value() == digest_before.value(),
        "game-owned condition state never enters the canonical document digest");
    require(record.serialize().find("CARTOGRAPHER_PHYSICAL_SURFACE_CONDITION_STATE") ==
            std::string::npos,
        "canonical twin form never embeds game-owned condition records");

    // Round-tripping drops the game-owned state: it is not authoring content.
    const auto decoded =
        carto::physical::PhysicalTwinRecord::deserialize(record.serialize());
    require(static_cast<bool>(decoded), "twin document round-trips");
    require(decoded.value().condition_states.empty(),
        "condition state does not round-trip through the canonical document");
    require(decoded.value().canonical_digest().value() == digest_before.value(),
        "round-tripped twin reproduces the identical digest");
}

void conservation_checks_pass_and_fail() {
    const auto prior = twin(1000.0);

    const auto first =
        carto::physical::evaluate_physical_twin_acceptance(prior, nullptr);
    require(static_cast<bool>(first), "first acceptance evaluates");
    require(first.value().accepted && first.value().first_acceptance,
        "first acceptance is accepted without prior state");
    require(first.value().evidence_digests_bound,
        "first acceptance binds exact evidence digests");
    require(first.value().game_owned_condition_records == 1U,
        "acceptance receipt observes the game-owned record count");

    // Same source digest, heavier candidate: conservation violation.
    auto heavier = twin(1500.0);
    heavier.source_digest = prior.source_digest;
    const auto violated =
        carto::physical::evaluate_physical_twin_acceptance(heavier, &prior);
    require(static_cast<bool>(violated), "conservation violation still evaluates");
    require(!violated.value().accepted, "mass increase without authorship is refused");
    require(!violated.value().mass_conservation_observed &&
            violated.value().volume_conservation_observed,
        "the violated axis is reported exactly");
    require(!violated.value().violations.empty(), "violation reasons are recorded");
    require(violated.value().prior_twin_digest.has_value() &&
            violated.value().prior_twin_digest.value() ==
                prior.canonical_digest().value(),
        "violation receipt binds the exact prior twin digest");

    // Authored change explains the increase: conservation passes.
    auto authored = twin(1500.0);
    authored.source_digest = digest("source-blob-v2");
    const auto explained =
        carto::physical::evaluate_physical_twin_acceptance(authored, &prior);
    require(static_cast<bool>(explained), "authored change still evaluates");
    require(explained.value().accepted &&
            explained.value().mass_conservation_observed,
        "authored change conserves mass lawfully");
}

void derived_values_bind_exact_evidence_digests() {
    auto record = twin(1000.0);
    record.mass.geometry_evidence_digest = digest("tampered-evidence");
    const auto rejected =
        carto::physical::evaluate_physical_twin_acceptance(record, nullptr);
    require(!rejected &&
            rejected.error().code == carto::core::ErrorCode::stale_data,
        "derived mass with mismatched evidence digest fails as stale_data");

    auto pressure_tampered = twin(1000.0);
    pressure_tampered.ground_pressure->mass_receipt_digest =
        digest("tampered-mass");
    const auto pressure_rejected = carto::physical::evaluate_physical_twin_acceptance(
        pressure_tampered, nullptr);
    require(!pressure_rejected &&
            pressure_rejected.error().code == carto::core::ErrorCode::stale_data,
        "ground pressure with mismatched mass digest fails as stale_data");
}

void twin_hostile_inputs_fail_closed() {
    const auto record = twin(1000.0);
    const std::string encoded = record.serialize();

    require(!carto::physical::PhysicalTwinRecord::deserialize(""),
        "empty twin text is rejected");
    require(!carto::physical::PhysicalTwinRecord::deserialize(encoded + "trailing"),
        "twin parser rejects trailing unbound data");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_PHYSICAL_TWIN 1"}.size(),
        "CARTOGRAPHER_PHYSICAL_TWIN 99");
    const auto future_result =
        carto::physical::PhysicalTwinRecord::deserialize(future);
    require(!future_result &&
            future_result.error().code == carto::core::ErrorCode::version_mismatch,
        "unknown future twin versions fail with version_mismatch");

    // Hostile oversized collection count inside the embedded ledger section:
    // swap the real count for one past the ledger bound.
    std::string oversized = encoded;
    const std::string hostile_count = "ENTRIES 65537\n";
    const std::string real_count = "ENTRIES 1\n";
    const auto count_position = oversized.find(real_count);
    require(count_position != std::string::npos, "fixture embeds its ledger count");
    oversized.replace(count_position, real_count.size(), hostile_count);
    require(!carto::physical::PhysicalTwinRecord::deserialize(oversized),
        "hostile nested ledger counts are rejected before allocation");
    require(!carto::physical::PhysicalTwinAcceptanceReceipt::deserialize("noise"),
        "acceptance receipt parser rejects garbage");
}

} // namespace

int main() {
    game_owned_condition_state_is_fenced_from_the_digest();
    conservation_checks_pass_and_fail();
    derived_values_bind_exact_evidence_digests();
    twin_hostile_inputs_fail_closed();
    std::cout << "physical twin tests passed\n";
    return EXIT_SUCCESS;
}
