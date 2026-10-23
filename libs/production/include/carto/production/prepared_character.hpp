#pragma once

// Character prepared-package producer contract (spec ch18 + ch21).
//
// This header is the Cartographer-side producer half of the character
// package bridge: it composes the per-product payloads of one character
// (skeleton, packed weights, optional lowered weights, morphs, clips,
// garment/groom secondary descriptors), binds them into a
// character::CharacterPreparedManifest closure, gates packaging on a
// per-product license grant ledger (ch21), and re-verifies a staged package
// independently of the composer. The VANTA-side decoder and runtime admission
// live in another repository; the byte formats produced here are the golden
// contract that decoder implements against.
//
// Release level: structural verification with CPU-reference evidence only.
// Nothing here claims native-device or integrated-product maturity.

#include <carto/character/definition.hpp>
#include <carto/character/manifest.hpp>
#include <carto/character/morph.hpp>
#include <carto/character/motion_document.hpp>
#include <carto/character/receipt.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/kinematic/lowering.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/production/prepared_publication.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

// Contract identifiers (ch18 required product kinds). These are the manifest
// `kind` strings; the parallel PreparedProductKind enumerators
// (character_skeleton, character_weights, character_morphs, character_clips,
// character_secondary, plus character_manifest for the staged closure file)
// let the same products ride the prepared-scene publication machinery.
inline constexpr std::string_view kCharacterManifestProductIdentity = "character.manifest";
inline constexpr std::string_view kCharacterManifestFileName = "manifest.carto-character";
inline constexpr std::string_view kCharacterSkeletonProductKind = "character.skeleton.v1";
inline constexpr std::string_view kCharacterSkinProductKind = "character.skin.v1";
inline constexpr std::string_view kCharacterMorphsProductKind = "character.morphs.v1";
inline constexpr std::string_view kCharacterMotionProductKind = "character.motion.v1";
inline constexpr std::string_view kCharacterSimulationProductKind = "character.simulation.v1";

// Per-product format schema carried in every manifest product record.
inline constexpr std::uint32_t kCharacterProductSchemaMajor = 1U;
inline constexpr std::uint32_t kCharacterProductSchemaMinor = 0U;

// Exact interchange coordinate summary the VANTA admission profile accepts:
// right-handed, +Y up, +Z forward, 1 meter per unit.
core::Result<void> check_vanta_coordinate(const character::ManifestCoordinate& coordinate);

// ---------------------------------------------------------------------------
// ch21 provenance / rights gate
// ---------------------------------------------------------------------------

// Origin category of the content one product was built from (ch21 evidence
// labels). `unreviewed` can never be packaged: it is an explicit refusal.
enum class CharacterLicenseOrigin : std::uint8_t {
    internal_reuse,
    internal_new,
    standards_interface,
    third_party_approved,
    unreviewed,
};

// Redistribution clearance is tracked separately from commercial-use
// clearance (ch21): public binaries and the private VANTA runtime may carry
// different distribution rights.
enum class CharacterRedistribution : std::uint8_t {
    public_redistribution,
    private_only,
    none,
};

enum class CharacterCommercialUse : std::uint8_t {
    cleared,
    not_cleared,
};

[[nodiscard]] std::string_view character_license_origin_name(CharacterLicenseOrigin origin) noexcept;
[[nodiscard]] std::string_view character_redistribution_name(CharacterRedistribution value) noexcept;
[[nodiscard]] std::string_view character_commercial_use_name(CharacterCommercialUse value) noexcept;

// One license grant for one prepared product. The grant is pinned to the
// exact source content digest the product derives from (a digest the caller
// already holds: rig graph, mesh, clip, morph source, cache source), so a
// stale source invalidates the grant before any packaging happens.
struct CharacterProductLicenseGrant {
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxPartyBytes = 256U;
    static constexpr std::size_t kMaxAttributionBytes = 1'024U;

    std::string product_identity;
    CharacterLicenseOrigin origin = CharacterLicenseOrigin::unreviewed;
    CharacterRedistribution redistribution = CharacterRedistribution::none;
    CharacterCommercialUse commercial_use = CharacterCommercialUse::not_cleared;
    bool attribution_required = false;
    std::string rights_holder;
    std::string attribution;
    assets::Sha256Digest source_content_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
};

// Fail-closed license gate over the exact product set about to be packaged,
// keyed by producer product identity and pinned to each product's declared
// source digest. Refuses (never warns) when: a product has no grant, a grant
// names a product that does not exist, two grants cover one product, the
// grant digest does not match the product's source digest, the origin is
// unreviewed, redistribution is `none`, public redistribution lacks
// commercial clearance, or required attribution is missing.
[[nodiscard]] core::Result<void> check_character_product_licenses(
    const std::map<std::string, assets::Sha256Digest>& product_source_digests,
    std::span<const CharacterProductLicenseGrant> grants);

// ---------------------------------------------------------------------------
// Composition inputs
// ---------------------------------------------------------------------------

// The canonical 8-influence packed weights product plus its admission record.
// The binding is the product payload (its own canonical codec); the layer
// record must be fresh and bound to the same joint graph.
struct CharacterWeightsProductInput {
    kinematic::SkinBinding binding;
    character::SkinWeightLayer layer;
};

// Optional 4-influence canonical lowering produced and certified elsewhere.
// A negative or non-finite certified error means "not certified" and refuses
// packaging: shipping an uncertified lowering would mislabel quality.
struct CharacterLoweredWeightsInput {
    std::vector<std::uint8_t> product_bytes;
    double certified_max_abs_error = -1.0;
};

// One morph target product: the immutable delta bytes inline in the product
// payload, pinned through the MorphTarget record.
struct CharacterMorphProductInput {
    character::MorphTarget target;
    std::vector<std::uint8_t> delta_blob;
};

// One clip product: opaque canonical clip bytes pinned through a ClipRef.
struct CharacterClipProductInput {
    character::ClipRef source;
    std::vector<std::uint8_t> clip_bytes;
};

// One garment/groom cache descriptor product (ch18 character.simulation.v1).
// `source_digest` pins the cache descriptor to the exact simulation source
// it was baked from.
struct CharacterSecondaryProductInput {
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxKindBytes = 64U;

    std::string identity;
    std::string descriptor_kind;
    std::vector<std::uint8_t> descriptor_bytes;
    assets::Sha256Digest source_digest;
};

// Everything compose_prepared_character needs. All collections are sorted and
// unique by construction: morphs by target id, clips by ClipRef, secondary
// products by identity, capabilities lexicographically.
struct PreparedCharacterInput {
    static constexpr std::size_t kMaxMorphs = 4'096U;
    static constexpr std::size_t kMaxClips = 4'096U;
    static constexpr std::size_t kMaxSecondary = 256U;
    static constexpr std::size_t kMaxCapabilities =
        character::CharacterPreparedManifest::kMaxCapabilities;
    static constexpr std::size_t kMaxBuildIdentityBytes = 256U;

    // Immutable authoring truth snapshot (validated and digest-bound).
    character::CharacterDefinition definition;
    // Deforming subgraph lowered to the skeleton product; its graph digest
    // must equal definition.rig.graph_digest or the rig is stale and the
    // composition refuses.
    kinematic::SkeletalAssetDescriptor skeleton;
    CharacterWeightsProductInput weights;
    std::optional<CharacterLoweredWeightsInput> lowered_weights;
    std::vector<CharacterMorphProductInput> morphs;
    std::vector<CharacterClipProductInput> clips;
    std::vector<CharacterSecondaryProductInput> secondary;
    // Exactly one grant per emitted product; checked before anything packs.
    std::vector<CharacterProductLicenseGrant> licenses;
    std::string target_profile_id;
    assets::Sha256Digest target_profile_digest;
    // Declared conversion from the authoring coordinate policy to the VANTA
    // profile. Identity conversions need no checklist; any non-identity
    // conversion with an incomplete checklist fails closed here.
    character::CoordinateConversion coordinate_conversion;
    // Producer identity recorded in per-product derivation receipts.
    std::string producer_build;
    std::vector<std::string> required_capabilities;
};

// The composed package: the closure manifest, the exact product payloads in
// manifest path order, and one derivation receipt per product.
struct PreparedCharacterPackage {
    character::CharacterPreparedManifest manifest;
    std::vector<PreparedProductPayload> payloads;
    std::vector<character::CharacterProductReceipt> receipts;
};

// Composes one immutable prepared character package. Validates every source
// binding (stale digests fail closed), builds the product payloads in
// deterministic order, runs the license gate, and emits the manifest with its
// closure verified before returning. No wall clock or unordered iteration
// enters any canonical byte.
[[nodiscard]] core::Result<PreparedCharacterPackage> compose_prepared_character(
    const PreparedCharacterInput& input);

// ---------------------------------------------------------------------------
// Independent verification (VANTA-decoder-side discipline, producer checked)
// ---------------------------------------------------------------------------

// Re-reads manifest bytes and re-checks the full closure against caller-
// supplied payload digests, then enforces the VANTA coordinate policy. This
// path deliberately shares no state with compose_prepared_character.
[[nodiscard]] core::Result<character::CharacterPreparedManifest> verify_prepared_character(
    std::string_view manifest_bytes,
    const std::map<std::string, assets::Sha256Digest>& payload_digests);

// Re-reads a staged package directory: the character manifest at
// "<dir>/manifest.carto-character", products at their declared relative
// paths. The prepared-scene envelope file ("manifest.carto-prepared") that
// binds the directory into the CAS selector is tolerated but not part of the
// character closure. Undeclared files, missing files, symlinks, digest
// drift and any non-VANTA coordinate refuse.
[[nodiscard]] core::Result<character::CharacterPreparedManifest> verify_prepared_character_package(
    const std::filesystem::path& package_dir);

} // namespace carto::production
