#include <carto/archetype/identity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "text_format.hpp"

namespace carto::archetype {
namespace {

constexpr std::string_view kMagic = "CARTO_INSTANCE_IDENTITY_SIDECAR";
constexpr std::string_view kDerivationPrefix = "CARTO_ARCHETYPE_INSTANCE_ID_V1";
constexpr std::string_view kSuccessorPrefix = "CARTO_ARCHETYPE_INSTANCE_ID_SUCCESSOR_V1";

std::uint64_t truncate_id(const assets::Sha256Digest& digest) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(digest.bytes[index]);
    }
    return value == 0U ? 1U : value;
}

std::uint64_t successor_instance_id(std::uint64_t identity) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(48U);
    detail::append_text(bytes, kSuccessorPrefix);
    detail::append_u64be(bytes, identity);
    return truncate_id(detail::digest_bytes(bytes));
}

} // namespace

std::uint64_t derive_instance_id(
    const assets::Sha256Digest& recipe_digest,
    std::uint64_t field_identity,
    std::uint64_t seed,
    std::uint64_t ordinal) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(96U);
    detail::append_text(bytes, kDerivationPrefix);
    detail::append_digest(bytes, recipe_digest);
    detail::append_u64be(bytes, field_identity);
    detail::append_u64be(bytes, seed);
    detail::append_u64be(bytes, ordinal);
    return truncate_id(detail::digest_bytes(bytes));
}

core::Result<void> InstanceIdentitySidecar::FieldMap::validate() const {
    if (!field) {
        return core::Result<void>::failure(detail::invalid_text(
            "identity sidecar field map has no field identity"));
    }
    if (recipe_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid_text(
            "identity sidecar field map has no recipe evidence"));
    }
    if (live_ids.size() > kMaxLiveIdsPerField || dead_ids.size() > kMaxDeadIdsPerField) {
        return core::Result<void>::failure(detail::invalid_text(
            "identity sidecar field map exceeds its bounded capacity"));
    }
    if (!std::is_sorted(dead_ids.begin(), dead_ids.end()) ||
        std::adjacent_find(dead_ids.begin(), dead_ids.end()) != dead_ids.end() ||
        std::any_of(dead_ids.begin(), dead_ids.end(),
            [](std::uint64_t value) { return value == 0U; })) {
        return core::Result<void>::failure(detail::invalid_text(
            "identity sidecar dead identities must be non-zero, sorted, and unique"));
    }
    const std::set<std::uint64_t> dead(dead_ids.begin(), dead_ids.end());
    std::set<std::uint64_t> seen;
    for (const auto& [ordinal, identity] : live_ids) {
        if (ordinal > kMaxTrackedOrdinal || identity == 0U ||
            !seen.insert(identity).second || dead.contains(identity)) {
            return core::Result<void>::failure(detail::invalid_text(
                "identity sidecar live identities must be unique, non-zero, and never dead"));
        }
    }
    return core::Result<void>::success();
}

std::string InstanceIdentitySidecar::FieldMap::canonical() const {
    std::ostringstream output;
    output << "FIELD " << field.value << ' ' << seed << ' ' << recipe_digest.hex() << ' '
           << live_ids.size();
    for (const auto& [ordinal, identity] : live_ids) {
        output << ' ' << ordinal << ' ' << identity;
    }
    output << ' ' << dead_ids.size();
    for (const std::uint64_t identity : dead_ids) {
        output << ' ' << identity;
    }
    return output.str();
}

core::Result<void> InstanceIdentitySidecar::validate() const {
    if (fields.size() > kMaxSidecarFields) {
        return core::Result<void>::failure(detail::invalid_text(
            "identity sidecar exceeds its bounded field capacity"));
    }
    for (const auto& [identity, map] : fields) {
        if (identity != map.field) {
            return core::Result<void>::failure(detail::invalid_text(
                "identity sidecar field map identity does not match its record"));
        }
        if (auto result = map.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string InstanceIdentitySidecar::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "FIELDS " << fields.size() << '\n';
    for (const auto& [identity, map] : fields) {
        static_cast<void>(identity);
        output << map.canonical() << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> InstanceIdentitySidecar::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(
        detail::digest_text(serialize()));
}

core::Result<InstanceIdentitySidecar> InstanceIdentitySidecar::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedSidecarBytes) {
        return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
            "identity sidecar text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_integer(input, version) ||
        magic != kMagic || version != kSchemaVersion) {
        return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
            "identity sidecar framing or schema version is invalid"));
    }

    InstanceIdentitySidecar sidecar;
    std::size_t field_count = 0U;
    if (!detail::require_token(input, "FIELDS") ||
        !detail::read_count(input, kMaxSidecarFields, field_count)) {
        return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
            "identity sidecar field header is malformed"));
    }
    for (std::size_t index = 0U; index < field_count; ++index) {
        FieldMap map;
        std::size_t live_count = 0U;
        std::size_t dead_count = 0U;
        if (!detail::require_token(input, "FIELD") ||
            !detail::read_integer(input, map.field.value) ||
            !detail::read_integer(input, map.seed)) {
            return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
                "identity sidecar field record is malformed"));
        }
        const auto recipe = detail::read_digest(
            input, "identity sidecar field", "recipe digest");
        if (!recipe) {
            return core::Result<InstanceIdentitySidecar>::failure(recipe.error());
        }
        map.recipe_digest = recipe.value();
        if (!detail::read_count(input, kMaxLiveIdsPerField, live_count)) {
            return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
                "identity sidecar live identity count is malformed"));
        }
        for (std::size_t live_index = 0U; live_index < live_count; ++live_index) {
            std::uint64_t ordinal = 0U;
            std::uint64_t identity = 0U;
            if (!detail::read_integer(input, ordinal) ||
                !detail::read_integer(input, identity) ||
                !map.live_ids.emplace(ordinal, identity).second) {
                return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
                    "identity sidecar live identity is malformed or duplicated"));
            }
        }
        if (!detail::read_count(input, kMaxDeadIdsPerField, dead_count)) {
            return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
                "identity sidecar dead identity count is malformed"));
        }
        for (std::size_t dead_index = 0U; dead_index < dead_count; ++dead_index) {
            std::uint64_t identity = 0U;
            if (!detail::read_integer(input, identity)) {
                return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
                    "identity sidecar dead identity is malformed"));
            }
            map.dead_ids.push_back(identity);
        }
        if (!sidecar.fields.emplace(map.field, std::move(map)).second) {
            return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
                "identity sidecar field identity is duplicated"));
        }
    }

    if (!detail::require_token(input, "END")) {
        return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
            "identity sidecar is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<InstanceIdentitySidecar>::failure(detail::malformed_text(
            "identity sidecar contains trailing unbound data"));
    }
    if (auto result = sidecar.validate(); !result) {
        return core::Result<InstanceIdentitySidecar>::failure(result.error());
    }
    return core::Result<InstanceIdentitySidecar>::success(std::move(sidecar));
}

core::Result<InstanceIdentitySidecar> reimport_instance_identities(
    const world::InstanceField& field,
    const InstanceIdentitySidecar* previous) {
    if (auto result = field.validate(); !result) {
        return core::Result<InstanceIdentitySidecar>::failure(result.error());
    }
    if (previous) {
        if (auto result = previous->validate(); !result) {
            return core::Result<InstanceIdentitySidecar>::failure(result.error());
        }
    }
    if (field.maximum_instances > kMaxLiveIdsPerField) {
        return core::Result<InstanceIdentitySidecar>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "instance field population exceeds the identity sidecar capacity"));
    }

    const InstanceIdentitySidecar::FieldMap* old_map = nullptr;
    if (previous) {
        const auto found = previous->fields.find(field.id);
        if (found != previous->fields.end()) old_map = &found->second;
    }

    const std::set<std::uint64_t> suppressed(
        field.suppressed_instances.begin(), field.suppressed_instances.end());
    std::set<std::uint64_t> dead;
    if (old_map) {
        dead.insert(old_map->dead_ids.begin(), old_map->dead_ids.end());
    }

    // Carry surviving identities forward. An identity the source now
    // suppresses transitions to dead and is dropped from the live map.
    std::map<std::uint64_t, std::uint64_t> carried;
    if (old_map) {
        for (const auto& [ordinal, identity] : old_map->live_ids) {
            if (suppressed.contains(identity)) {
                dead.insert(identity);
            } else {
                carried.emplace(ordinal, identity);
            }
        }
    }

    InstanceIdentitySidecar::FieldMap next;
    next.field = field.id;
    next.seed = field.seed;
    next.recipe_digest = field.recipe_digest;

    for (std::uint64_t ordinal = 0U; ordinal < field.maximum_instances; ++ordinal) {
        const std::uint64_t derived = derive_instance_id(
            field.recipe_digest, field.id.value, field.seed, ordinal);
        if (suppressed.contains(derived)) {
            dead.insert(derived);
            continue;
        }
        const auto carried_entry = carried.find(ordinal);
        if (carried_entry != carried.end()) {
            if (!dead.contains(carried_entry->second)) {
                next.live_ids.emplace(ordinal, carried_entry->second);
            }
            continue;
        }
        std::uint64_t identity = derived;
        std::size_t attempts = 0U;
        while (dead.contains(identity)) {
            identity = successor_instance_id(identity);
            if (++attempts > kMaxIdCollisionAttempts) {
                return core::Result<InstanceIdentitySidecar>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "instance identity space is exhausted against the dead set"));
            }
        }
        next.live_ids.emplace(ordinal, identity);
    }

    // Ordinals retired by population shrink stay mapped so a later regrowth
    // reuses their identities instead of minting new ones.
    for (const auto& [ordinal, identity] : carried) {
        if (ordinal >= field.maximum_instances && !dead.contains(identity)) {
            next.live_ids.emplace(ordinal, identity);
        }
    }

    next.dead_ids.assign(dead.begin(), dead.end());
    if (auto result = next.validate(); !result) {
        return core::Result<InstanceIdentitySidecar>::failure(result.error());
    }

    InstanceIdentitySidecar sidecar = previous ? *previous : InstanceIdentitySidecar{};
    sidecar.fields[field.id] = std::move(next);
    if (auto result = sidecar.validate(); !result) {
        return core::Result<InstanceIdentitySidecar>::failure(result.error());
    }
    return core::Result<InstanceIdentitySidecar>::success(std::move(sidecar));
}

} // namespace carto::archetype
