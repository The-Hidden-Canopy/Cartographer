#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/types.hpp>
#include <carto/surface/core.hpp>
#include <carto/surface/process.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace carto::physical {

// CONDITION-style damage region bound to a surface domain. The stratigraphy
// copy is authoring evidence: which material is exposed at a given accumulated
// damage depth is resolved through carto::surface::resolve_material_exposure,
// so damage consumers observe surface truth instead of re-deriving it.
struct DamageSurfaceDomain {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxClassBytes = 128U;

    surface::SurfaceDomainId domain;
    core::Revision source_revision;
    // Authoring evidence: the domain stratigraphy. It is also the single
    // source of identity on the wire: deserialization restores domain and
    // source_revision from this record, never from a second copy.
    surface::MaterialStratigraphy stratigraphy;
    // Stable game-facing damage region identity. A number, not a handle.
    std::uint32_t damage_region = 0U;
    std::string damage_class;

    [[nodiscard]] core::Result<void> validate() const;
    // Which material a consumer observes at the given accumulated damage
    // depth, resolved by the surface library.
    [[nodiscard]] core::Result<surface::MaterialExposure> exposure(
        double damage_depth) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<DamageSurfaceDomain> deserialize(
        std::string_view text);
};

// Shelter semantics for safe-space style domains: which material forms the
// sheltered (outermost) surface and how completely the domain covers what is
// beneath it. Shelter class is neutral authoring vocabulary, not game policy.
struct ShelterSurfaceDomain {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxClassBytes = 128U;

    surface::SurfaceDomainId domain;
    core::Revision source_revision;
    // Authoring evidence and single source of identity on the wire (see
    // DamageSurfaceDomain).
    surface::MaterialStratigraphy stratigraphy;
    std::string shelter_class;
    // Authored coverage factor in [0, 1]: 1 fully shelters what is beneath.
    double coverage = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    // The material at the sheltered surface (exposure depth zero).
    [[nodiscard]] core::Result<surface::MaterialExposure> sheltered_material() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ShelterSurfaceDomain> deserialize(
        std::string_view text);
};

// Consumer bindings for one surface domain. Every field is optional because
// not every domain participates in every consumer; at least one binding must
// be present for the entry to earn ledger membership. Identities are stable
// strings or authoring region numbers, never runtime handles.
struct PhysicalSurfaceEntry {
    surface::SurfaceDomainId domain;
    core::Revision source_revision;
    // Render material region in the prepared product.
    std::optional<std::uint32_t> render_material_region;
    // VANTA physics surface identity, e.g. "vanta.physics.surfaces/wood-rough".
    std::optional<std::string> physics_surface_identity;
    // CONDITION-style damage region number.
    std::optional<std::uint32_t> damage_region;
    // VANTA audio surface identity.
    std::optional<std::string> audio_surface_identity;
    // Weather exposure evidence: the authoring stratigraphy this domain's
    // weathering starts from.
    std::optional<surface::MaterialStratigraphy> weather_exposure;

    [[nodiscard]] core::Result<void> validate() const;
};

// Maps surface domain identity to its consumer bindings across rendering,
// physics, damage, audio, and weather. The ledger is a canonical, digest-bound
// document: entries are keyed and ordered by domain identity, so the same
// bindings always produce the same digest regardless of insertion order.
class PhysicalSurfaceLedger final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxEntries = 65'536U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;

    [[nodiscard]] core::Result<void> insert(PhysicalSurfaceEntry entry);
    [[nodiscard]] const std::map<surface::SurfaceDomainId, PhysicalSurfaceEntry>&
    entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PhysicalSurfaceLedger> deserialize(
        std::string_view text);

private:
    std::map<surface::SurfaceDomainId, PhysicalSurfaceEntry> entries_;
};

namespace detail {
void encode_ledger(std::ostream& output, const PhysicalSurfaceLedger& ledger);
core::Result<void> decode_ledger(std::istream& input, PhysicalSurfaceLedger& ledger);
} // namespace detail

} // namespace carto::physical
