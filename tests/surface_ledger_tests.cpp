#include <carto/physical/surface_ledger.hpp>

#include <cstdlib>
#include <iostream>
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

carto::surface::MaterialStratigraphy stratigraphy(
    carto::surface::SurfaceDomainId domain) {
    carto::surface::MaterialStratigraphy value;
    value.domain = domain;
    value.source_revision = carto::core::Revision{1U};
    value.layers = {
        {42U, 0.05, 0.0, "paint"},
        {43U, 0.20, 0.0, "plaster"},
    };
    require(static_cast<bool>(value.refresh_digest()),
        "test stratigraphy refreshes its digest");
    return value;
}

void damage_and_shelter_domains_resolve_exposure() {
    const auto layers = stratigraphy(carto::surface::SurfaceDomainId{7U});

    carto::physical::DamageSurfaceDomain damage;
    damage.domain = carto::surface::SurfaceDomainId{7U};
    damage.source_revision = carto::core::Revision{1U};
    damage.stratigraphy = layers;
    damage.damage_region = 11U;
    damage.damage_class = "condition/motel8/wall";
    require(static_cast<bool>(damage.validate()), "damage domain validates");

    const auto shallow = damage.exposure(0.03);
    require(static_cast<bool>(shallow), "shallow damage resolves exposure");
    require(shallow.value().material == 42U && shallow.value().layer_index == 0U,
        "shallow damage exposes the outer layer material");
    require(!shallow.value().reaches_substrate,
        "shallow damage does not reach the substrate");

    const auto deep = damage.exposure(0.15);
    require(static_cast<bool>(deep), "deep damage resolves exposure");
    require(deep.value().material == 43U && deep.value().layer_index == 1U,
        "deep damage exposes the inner layer material");
    require(!deep.value().reaches_substrate,
        "in-range damage still stays inside the stratigraphy");

    const auto through = damage.exposure(5.0);
    require(static_cast<bool>(through), "through damage resolves exposure");
    require(through.value().reaches_substrate,
        "damage past the stratigraphy reports the substrate");

    carto::physical::ShelterSurfaceDomain shelter;
    shelter.domain = carto::surface::SurfaceDomainId{8U};
    shelter.source_revision = carto::core::Revision{1U};
    shelter.stratigraphy = stratigraphy(carto::surface::SurfaceDomainId{8U});
    shelter.shelter_class = "roofed";
    shelter.coverage = 1.0;
    require(static_cast<bool>(shelter.validate()), "shelter domain validates");
    const auto sheltered = shelter.sheltered_material();
    require(static_cast<bool>(sheltered), "shelter resolves its surface material");
    require(sheltered.value().material == 42U && sheltered.value().layer_index == 0U,
        "shelter exposes the outermost layer material");

    const auto round_trip = carto::physical::DamageSurfaceDomain::deserialize(
        damage.serialize());
    require(static_cast<bool>(round_trip), "damage domain round-trips");
    require(round_trip.value().canonical_digest().value() ==
            damage.canonical_digest().value(),
        "damage domain digest survives serialization");

    shelter.coverage = 1.5;
    require(!shelter.validate(), "shelter coverage outside [0, 1] is rejected");
    damage.damage_region = 0U;
    require(!damage.validate(), "zero damage region is rejected");
}

void ledger_digest_is_order_stable() {
    carto::physical::PhysicalSurfaceLedger first;
    require(static_cast<bool>(first.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{5U},
                carto::core::Revision{1U},
                std::uint32_t{3U},
                std::string{"consumer.physics.surfaces/concrete-smooth"},
                std::uint32_t{11U},
                std::string{"consumer.audio.surfaces/concrete"},
                std::nullopt})),
        "first ledger entry inserts");
    require(static_cast<bool>(first.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{9U},
                carto::core::Revision{1U},
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::string{"consumer.audio.surfaces/metal-hollow"},
                stratigraphy(carto::surface::SurfaceDomainId{9U})})),
        "second ledger entry inserts");

    carto::physical::PhysicalSurfaceLedger second;
    require(static_cast<bool>(second.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{9U},
                carto::core::Revision{1U},
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::string{"consumer.audio.surfaces/metal-hollow"},
                stratigraphy(carto::surface::SurfaceDomainId{9U})})),
        "reversed ledger entry inserts");
    require(static_cast<bool>(second.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{5U},
                carto::core::Revision{1U},
                std::uint32_t{3U},
                std::string{"consumer.physics.surfaces/concrete-smooth"},
                std::uint32_t{11U},
                std::string{"consumer.audio.surfaces/concrete"},
                std::nullopt})),
        "reversed first ledger entry inserts");

    const auto first_digest = first.canonical_digest();
    const auto second_digest = second.canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest),
        "both ledger digests compute");
    require(first.serialize() == second.serialize(),
        "insertion order does not change the canonical ledger bytes");
    require(first_digest.value() == second_digest.value(),
        "insertion order does not change the ledger digest");

    const auto decoded =
        carto::physical::PhysicalSurfaceLedger::deserialize(first.serialize());
    require(static_cast<bool>(decoded), "ledger round-trips");
    require(decoded.value().canonical_digest().value() == first_digest.value(),
        "ledger digest survives a serialization round trip");

    require(!first.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{5U},
                carto::core::Revision{1U},
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt}),
        "duplicate ledger domain identities are rejected");
    require(!first.insert(carto::physical::PhysicalSurfaceEntry{
                carto::surface::SurfaceDomainId{12U},
                carto::core::Revision{1U},
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt}),
        "ledger entries without any consumer binding are rejected");
}

void ledger_hostile_inputs_fail_closed() {
    const std::string valid = "CARTOGRAPHER_PHYSICAL_SURFACE_LEDGER 1\n"
                              "ENTRIES 0\n"
                              "END\n";
    require(static_cast<bool>(
                carto::physical::PhysicalSurfaceLedger::deserialize(valid)),
        "an empty ledger is a valid document");

    require(!carto::physical::PhysicalSurfaceLedger::deserialize(""),
        "empty ledger text is rejected");
    require(!carto::physical::PhysicalSurfaceLedger::deserialize(valid + "trailing"),
        "ledger parser rejects trailing unbound data");

    std::string future = valid;
    future.replace(0U, std::string{"CARTOGRAPHER_PHYSICAL_SURFACE_LEDGER 1"}.size(),
        "CARTOGRAPHER_PHYSICAL_SURFACE_LEDGER 99");
    const auto future_result =
        carto::physical::PhysicalSurfaceLedger::deserialize(future);
    require(!future_result &&
            future_result.error().code == carto::core::ErrorCode::version_mismatch,
        "unknown future ledger versions fail with version_mismatch");

    require(!carto::physical::PhysicalSurfaceLedger::deserialize(
                "CARTOGRAPHER_PHYSICAL_SURFACE_LEDGER 1\nENTRIES 65537\n"),
        "hostile ledger counts are rejected before allocation");

    // Duplicate domain identities inside one document fail closed on insert.
    const std::string duplicate =
        "CARTOGRAPHER_PHYSICAL_SURFACE_LEDGER 1\n"
        "ENTRIES 2\n"
        "ENTRY 1\n"
        "DOMAIN 5\n"
        "RENDER_REGION 1 3\n"
        "PHYSICS_SURFACE 0 -\n"
        "DAMAGE_REGION 0 -\n"
        "AUDIO_SURFACE 0 -\n"
        "WEATHER_EXPOSURE 0\n"
        "ENTRY 1\n"
        "DOMAIN 5\n"
        "RENDER_REGION 1 4\n"
        "PHYSICS_SURFACE 0 -\n"
        "DAMAGE_REGION 0 -\n"
        "AUDIO_SURFACE 0 -\n"
        "WEATHER_EXPOSURE 0\n"
        "END\n";
    const auto duplicate_result =
        carto::physical::PhysicalSurfaceLedger::deserialize(duplicate);
    require(!duplicate_result &&
            duplicate_result.error().code == carto::core::ErrorCode::invalid_argument,
        "duplicate ledger domain identities inside one document are rejected");
}

} // namespace

int main() {
    damage_and_shelter_domains_resolve_exposure();
    ledger_digest_is_order_stable();
    ledger_hostile_inputs_fail_closed();
    std::cout << "surface ledger tests passed\n";
    return EXIT_SUCCESS;
}
