#pragma once

#include <carto/character/controls.hpp>
#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::face {

// The six facial regions named by the spec. They are semantic regions, not a
// fixed humanoid shape set: a region binds to whatever control set and morph
// group a character actually has, and a missing binding is unmapped, never
// fuzzy-matched.
enum class FacialRegion : std::uint8_t {
    brow,
    lid,
    cheek,
    jaw,
    lip,
    tongue,
};

// Laterality of one region binding. Bilateral bindings drive both sides.
enum class FacialSide : std::uint8_t {
    left,
    right,
    bilateral,
};

[[nodiscard]] std::string_view facial_region_name(FacialRegion region) noexcept;
[[nodiscard]] std::string_view facial_side_name(FacialSide side) noexcept;
[[nodiscard]] core::Result<FacialRegion> facial_region_from_name(std::string_view text);
[[nodiscard]] core::Result<FacialSide> facial_side_from_name(std::string_view text);

// One binding of a facial region/side to authoring state: the control set
// (character::ControlDefinition::control_set) that drives it and the named
// morph target group that shapes it. Both are stable authoring tokens; the
// schema that owns the binding is digest-bound to the exact control graph and
// morph target population it was authored against.
struct RegionBinding {
    static constexpr std::size_t kMaxTokenBytes = 128U;

    FacialRegion region = FacialRegion::brow;
    FacialSide side = FacialSide::bilateral;
    std::string control_set;
    std::string morph_group;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const RegionBinding&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const RegionBinding&) const noexcept = default;
};

// Versioned, digest-bound schema binding the facial regions of one character
// to its control sets and morph groups. Digest binding is the staleness gate:
// control_graph_digest and morph_library_digest must equal the digests
// recomputed from the live control graph and morph target population, and
// validation against live state refuses a schema whose digests disagree.
class FacialRegionSchema final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxBindings = 256U;
    static constexpr std::size_t kMaxSerializedBytes = 256U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    character::CharacterId character_id;
    assets::Sha256Digest control_graph_digest{};
    assets::Sha256Digest morph_library_digest{};
    // Sorted and unique by (region, side).
    std::vector<RegionBinding> bindings;

    [[nodiscard]] core::Result<void> insert(RegionBinding binding);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<FacialRegionSchema> deserialize(
        std::string_view text);
};

// Canonical digest over a morph target identity population: the sorted set of
// stable morph target ids, serialized in canonical form. A schema or
// expression authored against a changed population has a different digest and
// fails closed instead of silently deforming changed sources.
[[nodiscard]] core::Result<assets::Sha256Digest> morph_library_digest(
    std::span<const character::MorphTargetId> targets);

// Canonical digest over the control graph's durable authoring state. This is
// the digest a schema records in control_graph_digest; equal graphs digest
// equally and any control change invalidates bound schemas.
[[nodiscard]] core::Result<assets::Sha256Digest> control_graph_digest(
    const character::ControlGraph& controls);

// Admission of a schema against live authoring state. Every named control set
// must exist among the control definitions (nonempty control_set values), the
// character identity must match, and both digests must equal the digests
// recomputed from the live state. Any disagreement is an honest refusal with
// stale_data, never a silent remap.
[[nodiscard]] core::Result<void> validate_region_bindings(
    const FacialRegionSchema& schema,
    const character::ControlGraph& controls,
    std::span<const character::MorphTargetId> morph_targets);

} // namespace carto::face
