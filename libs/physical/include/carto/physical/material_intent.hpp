#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/types.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::physical {

// Reference to a stable downstream consumer physical-material catalog identity plus authored
// intent qualifiers. This is deliberately NOT a copied catalog entry: it
// carries no display name, no resolved physical constants, and no runtime
// handle. downstream consumer remains the sole owner of catalog truth; Cartographer only
// records which catalog identity the authoring intent points at and the
// qualifiers the author set.
struct PhysicalMaterialIntent {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxQualifierCount = 64U;
    static constexpr std::size_t kMaxQualifierKeyBytes = 128U;
    static constexpr std::size_t kMaxQualifierValueBytes = 256U;
    static constexpr double kMaxDensityKgPerM3 = 1.0e9;

    // Stable catalog identity, e.g. "consumer.physical.materials/steel-corten".
    std::string catalog_identity;
    // Catalog revision observed when the intent was authored. Zero means the
    // author did not pin a revision; the exhausted value is always invalid.
    core::Revision catalog_revision;
    // Optional authored density intent used by deterministic mass derivation.
    // When absent, mass derivation fails closed rather than substituting a
    // catalog constant that Cartographer does not own.
    std::optional<double> density_kg_per_m3;
    // Authoring qualifiers, sorted by key and unique. Keys are non-empty.
    std::vector<std::pair<std::string, std::string>> qualifiers;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PhysicalMaterialIntent> deserialize(
        std::string_view text);
};

namespace detail {
void encode_intent(std::ostream& output, const PhysicalMaterialIntent& intent);
core::Result<void> decode_intent(std::istream& input, PhysicalMaterialIntent& intent);
} // namespace detail

} // namespace carto::physical
