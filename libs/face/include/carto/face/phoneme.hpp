#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::face {

// Ceiling for one mapping entry's viseme weight.
inline constexpr double kMaxVisemeWeight = 8.0;

// The phoneme inventory for one locale: the sorted, unique, bounded set of
// phoneme tokens a speech alignment may reference. Versioned and
// digest-addressed; a changed set has a different canonical digest.
class PhonemeInventory final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxPhonemes = 512U;
    static constexpr std::size_t kMaxPhonemeBytes = 16U;
    static constexpr std::size_t kMaxLocaleBytes = 32U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    std::string locale; // e.g. "en-US"
    std::vector<std::string> phonemes; // sorted, unique

    [[nodiscard]] core::Result<void> insert(std::string phoneme);
    [[nodiscard]] bool contains(std::string_view phoneme) const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PhonemeInventory> deserialize(
        std::string_view text);
};

// One phoneme -> viseme mapping entry. `phoneme` must be an inventory member
// (enforced by VisemeMapping::validate_against); `weight` scales the viseme
// contribution and must be finite and within (0, kMaxVisemeWeight].
struct VisemeMappingEntry {
    std::string phoneme;
    std::string viseme;
    double weight = 1.0;

    [[nodiscard]] bool operator==(const VisemeMappingEntry&) const noexcept = default;
};

// Result of a successful phoneme lookup: the viseme name (a stable view into
// the mapping) and its strength weight.
struct VisemeMappingLookup {
    std::string_view viseme;
    double weight = 1.0;
};

// Versioned, digest-bound phoneme -> viseme table for one locale. The table is
// bound to its phoneme inventory by inventory_digest: a mapping admitted
// against an inventory whose digest disagrees fails closed, and a phoneme with
// no entry is an honest refusal (VisemeMapping::map), never a fuzzy guess.
class VisemeMapping final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxEntries = 512U;
    static constexpr std::size_t kMaxNameBytes = 128U;
    static constexpr std::size_t kMaxVisemeBytes = 64U;
    static constexpr std::size_t kMaxLocaleBytes = 32U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    std::string name; // e.g. "english-basic-16"
    std::string locale;
    assets::Sha256Digest inventory_digest{};
    std::vector<VisemeMappingEntry> entries; // sorted by phoneme, unique

    [[nodiscard]] core::Result<void> insert(VisemeMappingEntry entry);
    // Honest refusal: an unknown or unmapped phoneme is an error, never a
    // silently chosen viseme.
    [[nodiscard]] core::Result<VisemeMappingLookup> map(std::string_view phoneme) const;
    [[nodiscard]] core::Result<void> validate() const;
    // Admission against live state: the inventory digest must equal the
    // inventory's canonical digest and every entry phoneme must be an
    // inventory member.
    [[nodiscard]] core::Result<void> validate_against(const PhonemeInventory& inventory) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<VisemeMapping> deserialize(
        std::string_view text);
};

} // namespace carto::face
