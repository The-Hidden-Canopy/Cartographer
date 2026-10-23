#include <carto/archetype/identity.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/world/model.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <span>
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

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

using carto::archetype::InstanceIdentitySidecar;
using carto::archetype::derive_instance_id;
using carto::archetype::reimport_instance_identities;
using carto::world::InstanceField;
using carto::world::InstanceFieldId;

const carto::assets::Sha256Digest kRecipe = digest("barn-grid-recipe-v1");

InstanceField barn_field(std::uint64_t maximum, std::vector<std::uint64_t> suppressed) {
    InstanceField field;
    field.id = InstanceFieldId{7U};
    field.semantic_type = "farm-building";
    field.area = std::nullopt;
    field.bounds = {{0.0, 0.0, 0.0}, {40.0, 12.0, 20.0}};
    field.recipe_identity = "public/barn-grid/v1";
    field.recipe_digest = kRecipe;
    field.seed = 42U;
    field.maximum_instances = maximum;
    field.suppressed_instances = std::move(suppressed);
    return field;
}

const InstanceIdentitySidecar::FieldMap& field_map(
    const InstanceIdentitySidecar& sidecar) {
    const auto found = sidecar.fields.find(InstanceFieldId{7U});
    require(found != sidecar.fields.end(), "sidecar contains the barn field map");
    return found->second;
}

InstanceIdentitySidecar cook_or_die(
    const InstanceField& field,
    const InstanceIdentitySidecar* previous,
    const char* message) {
    const auto result = reimport_instance_identities(field, previous);
    require(result.has_value(), message);
    return result.value();
}

void initial_cook_derives_seed_bound_identities() {
    const InstanceField field = barn_field(4U, {});
    const InstanceIdentitySidecar sidecar = cook_or_die(field, nullptr,
        "initial cook succeeds");

    const auto& map = field_map(sidecar);
    require(map.live_ids.size() == 4U && map.dead_ids.empty(),
            "initial cook derives one live identity per ordinal");
    for (std::uint64_t ordinal = 0U; ordinal < 4U; ++ordinal) {
        const auto found = map.live_ids.find(ordinal);
        require(found != map.live_ids.end() &&
                    found->second == derive_instance_id(kRecipe, 7U, 42U, ordinal),
                "initial identities are the deterministic seed-bound derivation");
    }
    std::set<std::uint64_t> unique;
    for (const auto& [ordinal, identity] : map.live_ids) {
        static_cast<void>(ordinal);
        unique.insert(identity);
    }
    require(unique.size() == map.live_ids.size(),
            "derived identities are unique");

    // A second cook over the same source is an exact fixed point.
    const InstanceIdentitySidecar again = cook_or_die(field, &sidecar,
        "re-cook over the same source succeeds");
    require(again.serialize() == sidecar.serialize(),
            "reimport of an unchanged field is an exact fixed point");
    const auto first_digest = sidecar.canonical_digest();
    const auto second_digest = again.canonical_digest();
    require(first_digest && second_digest && first_digest.value() == second_digest.value(),
            "sidecar digest is stable across reimport");

    const std::string encoded = sidecar.serialize();
    const auto reopened = InstanceIdentitySidecar::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
            "identity sidecar survives deterministic round trip");
}

void suppressed_instances_stay_dead_across_reimport() {
    const InstanceField field = barn_field(4U, {});
    const InstanceIdentitySidecar cooked = cook_or_die(field, nullptr,
        "initial cook succeeds");
    const std::uint64_t killed = derive_instance_id(kRecipe, 7U, 42U, 2U);

    const InstanceField suppressed = barn_field(4U, {killed});
    const InstanceIdentitySidecar reimported = cook_or_die(suppressed, &cooked,
        "reimport with suppression succeeds");
    const auto& map = field_map(reimported);
    require(map.live_ids.size() == 3U && !map.live_ids.contains(2U),
            "suppressed ordinal is omitted from the live map");
    require(std::find(map.dead_ids.begin(), map.dead_ids.end(), killed) !=
                map.dead_ids.end(),
            "suppressed identity enters the dead set");
    for (std::uint64_t ordinal : {0U, 1U, 3U}) {
        const auto found = map.live_ids.find(ordinal);
        require(found != map.live_ids.end() &&
                    found->second == derive_instance_id(kRecipe, 7U, 42U, ordinal),
                "surviving identities are unchanged by suppression");
    }

    // Reimporting again must not resurrect the dead ordinal or identity.
    const InstanceIdentitySidecar again = cook_or_die(suppressed, &reimported,
        "second reimport succeeds");
    require(again.serialize() == reimported.serialize(),
            "suppressed instances are not resurrected by reimport");
    for (const auto& [ordinal, identity] : field_map(again).live_ids) {
        static_cast<void>(ordinal);
        require(identity != killed, "dead identity is never reissued");
    }

    // Population growth keeps surviving identities and avoids the dead one.
    const InstanceField grown = barn_field(6U, {killed});
    const InstanceIdentitySidecar regrown = cook_or_die(grown, &reimported,
        "regrowth reimport succeeds");
    const auto& regrown_map = field_map(regrown);
    require(regrown_map.live_ids.size() == 5U,
            "regrowth derives identities only for newly reachable ordinals");
    for (std::uint64_t ordinal : {0U, 1U, 3U}) {
        require(regrown_map.live_ids.at(ordinal) ==
                    derive_instance_id(kRecipe, 7U, 42U, ordinal),
                "regrowth preserves surviving identities");
    }
    for (std::uint64_t ordinal : {4U, 5U}) {
        const auto found = regrown_map.live_ids.find(ordinal);
        require(found != regrown_map.live_ids.end() && found->second != killed,
                "fresh identities deterministically avoid the dead set");
    }
}

void identity_survives_population_shrink_and_regrowth() {
    const InstanceField field = barn_field(4U, {});
    const InstanceIdentitySidecar cooked = cook_or_die(field, nullptr,
        "initial cook succeeds");

    const InstanceField shrunk = barn_field(2U, {});
    const InstanceIdentitySidecar shrink_reimport = cook_or_die(shrunk, &cooked,
        "shrink reimport succeeds");
    const auto& shrunk_map = field_map(shrink_reimport);
    require(shrunk_map.live_ids.size() == 4U,
            "retired ordinals stay mapped through population shrink");
    require(shrunk_map.live_ids.at(0U) == derive_instance_id(kRecipe, 7U, 42U, 0U) &&
                shrunk_map.live_ids.at(1U) == derive_instance_id(kRecipe, 7U, 42U, 1U),
            "shrunk population keeps the surviving identities");

    const InstanceIdentitySidecar regrown = cook_or_die(field, &shrink_reimport,
        "regrowth reimport succeeds");
    require(regrown.serialize() == cooked.serialize(),
            "regrowth restores the exact original identity map");
}

void dead_id_collisions_resolve_deterministically() {
    const std::uint64_t blocked = derive_instance_id(kRecipe, 7U, 42U, 0U);

    // Cook a field whose ordinal zero is suppressed: the derived identity
    // becomes dead without ever being issued.
    const InstanceField suppressing = barn_field(4U, {blocked});
    const InstanceIdentitySidecar blocked_cook = cook_or_die(suppressing, nullptr,
        "suppressing cook succeeds");
    require(!field_map(blocked_cook).live_ids.contains(0U),
            "suppressed ordinal is not issued during initial cook");

    // Revert the suppression: ordinal zero needs a fresh identity that is
    // deterministically not the dead one.
    const InstanceField restored = barn_field(4U, {});
    const InstanceIdentitySidecar restored_cook = cook_or_die(restored, &blocked_cook,
        "restoration reimport succeeds");
    const auto& map = field_map(restored_cook);
    require(map.live_ids.size() == 4U,
            "restoration reissues every reachable ordinal");
    require(map.live_ids.at(0U) != blocked,
            "reissued identity avoids the dead derivation");
    for (std::uint64_t ordinal : {1U, 2U, 3U}) {
        require(map.live_ids.at(ordinal) == derive_instance_id(kRecipe, 7U, 42U, ordinal),
                "unblocked ordinals keep their derivations");
    }
    const InstanceIdentitySidecar repeat = cook_or_die(restored, &blocked_cook,
        "repeat restoration succeeds");
    require(repeat.serialize() == restored_cook.serialize(),
            "collision resolution is deterministic across runs");
}

void sidecar_bytes_fail_closed() {
    const InstanceField field = barn_field(3U, {});
    const InstanceIdentitySidecar sidecar = cook_or_die(field, nullptr,
        "initial cook succeeds");
    const std::string encoded = sidecar.serialize();

    const auto trailing = InstanceIdentitySidecar::deserialize(encoded + "trailing");
    require(!trailing, "trailing sidecar data is rejected");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTO_INSTANCE_IDENTITY_SIDECAR 1"}.size(),
        "CARTO_INSTANCE_IDENTITY_SIDECAR 99");
    require(!InstanceIdentitySidecar::deserialize(future),
            "unknown future sidecar versions fail precisely");

    const std::string recipe_hex = kRecipe.hex();
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 65537\n"),
            "hostile field counts are rejected before allocation");
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 1\n"
                "FIELD 7 42 " + recipe_hex + " 1000001\n"),
            "hostile live identity counts are rejected before allocation");

    const std::uint64_t first = derive_instance_id(kRecipe, 7U, 42U, 0U);
    const std::uint64_t second = derive_instance_id(kRecipe, 7U, 42U, 1U);
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 1\n"
                "FIELD 7 42 " + recipe_hex + " 1 0 0\n"),
            "zero live identities are rejected");
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 1\n"
                "FIELD 7 42 " + recipe_hex + " 2 0 " +
                std::to_string(first) + " 1 " + std::to_string(first) + "\n"),
            "duplicated live identities are rejected");
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 1\n"
                "FIELD 7 42 " + recipe_hex + " 1 0 " + std::to_string(first) +
                " 1 " + std::to_string(first) + "\n"),
            "live identities may not overlap the dead set");
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 1\n"
                "FIELD 7 42 " + recipe_hex + " 1 0 " + std::to_string(second) +
                " 2 " + std::to_string(first) + " " + std::to_string(first) + "\n"),
            "unsorted dead identities are rejected");
    require(!InstanceIdentitySidecar::deserialize(
                "CARTO_INSTANCE_IDENTITY_SIDECAR 1\nFIELDS 1\n"
                "FIELD 7 42 " + recipe_hex + " 1 0 " + std::to_string(first) + "\n"
                "FIELD 7 42 " + recipe_hex + " 0 0\n"),
            "duplicated field identities are rejected");

    // Invalid source fields cannot be reimported at all.
    InstanceField invalid = barn_field(3U, {});
    invalid.maximum_instances = 0U;
    require(!reimport_instance_identities(invalid, nullptr),
            "reimport rejects an invalid source field");

    // Populations beyond the sidecar capacity fail closed instead of
    // minting an untracked identity map.
    InstanceField huge = barn_field(2U * 1000U * 1000U, {});
    require(!reimport_instance_identities(huge, nullptr),
            "populations beyond sidecar capacity are refused");
}

} // namespace

int main() {
    initial_cook_derives_seed_bound_identities();
    suppressed_instances_stay_dead_across_reimport();
    identity_survives_population_shrink_and_regrowth();
    dead_id_collisions_resolve_deterministically();
    sidecar_bytes_fail_closed();
    std::cout << "instance identity tests passed\n";
    return EXIT_SUCCESS;
}
