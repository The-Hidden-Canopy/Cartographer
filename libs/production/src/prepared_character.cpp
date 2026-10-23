#include <carto/production/prepared_character.hpp>

#include <carto/character/coordinate.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::production {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

assets::Sha256Digest digest_bytes(std::span<const std::uint8_t> bytes) {
    return assets::sha256(bytes);
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::vector<std::uint8_t> text_bytes(std::string_view text) {
    return std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        reinterpret_cast<const std::uint8_t*>(text.data()) + text.size());
}

std::string manifest_path_for(std::string_view kind_contract) {
    // kind_contract is "character.<name>.v1"; the product file keeps the full
    // contract spelling so the decoder never guesses from an extension.
    return "products/" + std::string(kind_contract);
}

// One product under construction, before the deterministic sort splits it
// into manifest records and payloads. `source_digest` is the exact source
// content the product derives from; the license gate pins grants to it.
struct CharacterProductDraft {
    std::string identity;
    std::string kind_contract;
    std::uint32_t schema = kCharacterProductSchemaMajor;
    std::string relative_path;
    std::vector<std::uint8_t> bytes;
    std::string recipe_identity;
    assets::Sha256Digest source_digest;
};

// Canonical morph product codec: one deterministic text header per target
// naming every MorphTarget record field, then that target's raw delta blob
// between explicit markers. Targets appear in sorted id order. The blob is
// opaque bytes; only its declared size and digest are trusted.
void append_morph_target(
    std::vector<std::uint8_t>& payload,
    const CharacterMorphProductInput& morph) {
    const std::string header = [&morph] {
        const character::MorphTarget& target = morph.target;
        std::ostringstream output;
        output << "TARGET " << target.id.value << '\n';
        output << "MESH_ASSET " << target.source_mesh_asset << '\n';
        output << "MESH_DIGEST " << target.source_mesh_digest.hex() << '\n';
        output << "DISPLACED " << target.displaced_point_count << '\n';
        output << "DELTA_DIGEST " << target.delta_blob_digest.hex() << '\n';
        output << "DELTA_BYTES " << target.delta_blob_bytes << '\n';
        output << "TOPOLOGY " << target.topology_correspondence_digest.hex() << '\n';
        output << "TANGENT_IN " << static_cast<unsigned>(target.tangent_in) << ' '
               << "TANGENT_OUT " << static_cast<unsigned>(target.tangent_out) << '\n';
        output << "PRECISION_BOUND " << std::setprecision(17) << target.precision_bound << '\n';
        output << "DELTA_BLOB_BEGIN " << target.delta_blob_bytes << '\n';
        return output.str();
    }();
    const auto header_bytes = text_bytes(header);
    payload.insert(payload.end(), header_bytes.begin(), header_bytes.end());
    payload.insert(payload.end(), morph.delta_blob.begin(), morph.delta_blob.end());
    const auto trailer_bytes = text_bytes("DELTA_BLOB_END\n");
    payload.insert(payload.end(), trailer_bytes.begin(), trailer_bytes.end());
}

core::Result<void> validate_morph_input(const CharacterMorphProductInput& morph) {
    if (auto result = morph.target.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("prepared character morph target"));
    }
    if (morph.delta_blob.size() != morph.target.delta_blob_bytes) {
        return core::Result<void>::failure(validation(
            "morph delta blob size does not match its declared record"));
    }
    if (digest_bytes(morph.delta_blob) != morph.target.delta_blob_digest) {
        return core::Result<void>::failure(stale(
            "morph delta blob digest does not match its declared record"));
    }
    return core::Result<void>::success();
}

// Bounds for the staged-package verifier. A character package is far below
// the scene bounds; keep them tight so hostile trees refuse early.
constexpr std::uintmax_t kMaxCharacterManifestBytes = 4U * 1024U * 1024U;
constexpr std::uintmax_t kMaxCharacterProductBytes = 512U * 1024U * 1024U;
constexpr std::uintmax_t kMaxCharacterPackageBytes = 1024U * 1024U * 1024U;

core::Result<std::vector<std::uint8_t>> read_bounded_file(
    const std::filesystem::path& path,
    std::uintmax_t maximum,
    std::string_view label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect prepared character " + std::string(label)));
    }
    if (std::filesystem::is_symlink(status)) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "prepared character paths cannot be symbolic links"));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "prepared character " + std::string(label) + " is missing or not a regular file"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "prepared character " + std::string(label) + " exceeds its bounded size"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to open prepared character " + std::string(label)));
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        (!input.good() && !input.eof())) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to read complete prepared character " + std::string(label)));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

} // namespace

core::Result<void> check_vanta_coordinate(const character::ManifestCoordinate& coordinate) {
    if (auto result = coordinate.validate(); !result) return result;
    if (coordinate.handedness != character::Handedness::right ||
        coordinate.up != character::Axis::positive_y ||
        coordinate.forward != character::Axis::positive_z ||
        coordinate.meters_per_unit != 1.0) {
        return core::Result<void>::failure(unsupported(
            "prepared character coordinate is not the VANTA admission profile "
            "(right-handed, +Y up, +Z forward, 1 meter per unit)"));
    }
    return core::Result<void>::success();
}

std::string_view character_license_origin_name(CharacterLicenseOrigin origin) noexcept {
    switch (origin) {
    case CharacterLicenseOrigin::internal_reuse: return "internal-reuse";
    case CharacterLicenseOrigin::internal_new: return "internal-new";
    case CharacterLicenseOrigin::standards_interface: return "standards-interface";
    case CharacterLicenseOrigin::third_party_approved: return "third-party-approved";
    case CharacterLicenseOrigin::unreviewed: return "unreviewed";
    }
    return "unknown";
}

std::string_view character_redistribution_name(CharacterRedistribution value) noexcept {
    switch (value) {
    case CharacterRedistribution::public_redistribution: return "public-redistribution";
    case CharacterRedistribution::private_only: return "private-only";
    case CharacterRedistribution::none: return "none";
    }
    return "unknown";
}

std::string_view character_commercial_use_name(CharacterCommercialUse value) noexcept {
    switch (value) {
    case CharacterCommercialUse::cleared: return "cleared";
    case CharacterCommercialUse::not_cleared: return "not-cleared";
    }
    return "unknown";
}

core::Result<void> CharacterProductLicenseGrant::validate() const {
    if (!safe_token(product_identity, kMaxIdentityBytes) ||
        !safe_token(rights_holder, kMaxPartyBytes) ||
        !safe_token(attribution, kMaxAttributionBytes, true) ||
        source_content_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "character license grant identity, party, attribution, or digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string CharacterProductLicenseGrant::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_CHARACTER_LICENSE_GRANT 1\n";
    output << "PRODUCT " << std::quoted(product_identity) << '\n';
    output << "ORIGIN " << character_license_origin_name(origin) << '\n';
    output << "REDISTRIBUTION " << character_redistribution_name(redistribution) << '\n';
    output << "COMMERCIAL " << character_commercial_use_name(commercial_use) << '\n';
    output << "ATTRIBUTION_REQUIRED " << (attribution_required ? 1U : 0U) << '\n';
    output << "RIGHTS_HOLDER " << std::quoted(rights_holder) << '\n';
    output << "ATTRIBUTION " << std::quoted(attribution) << '\n';
    output << "DIGEST " << source_content_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<void> check_character_product_licenses(
    const std::map<std::string, assets::Sha256Digest>& product_source_digests,
    std::span<const CharacterProductLicenseGrant> grants) {
    std::map<std::string, const CharacterProductLicenseGrant*> by_identity;
    for (const CharacterProductLicenseGrant& grant : grants) {
        if (auto result = grant.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("character product license grant"));
        }
        if (!by_identity.emplace(grant.product_identity, &grant).second) {
            return core::Result<void>::failure(validation(
                "character product has more than one license grant"));
        }
    }
    for (const auto& [identity, source_digest] : product_source_digests) {
        const auto grant = by_identity.find(identity);
        if (grant == by_identity.end()) {
            return core::Result<void>::failure(unsupported(
                "character product has no license grant: " + identity));
        }
        if (grant->second->source_content_digest != source_digest) {
            return core::Result<void>::failure(stale(
                "character license grant digest does not match the product source digest: " +
                identity));
        }
        const CharacterProductLicenseGrant& rights = *grant->second;
        if (rights.origin == CharacterLicenseOrigin::unreviewed) {
            return core::Result<void>::failure(unsupported(
                "character product origin is unreviewed and cannot be packaged: " +
                identity));
        }
        if (rights.redistribution == CharacterRedistribution::none) {
            return core::Result<void>::failure(unsupported(
                "character product has no redistribution clearance: " + identity));
        }
        if (rights.redistribution == CharacterRedistribution::public_redistribution &&
            rights.commercial_use == CharacterCommercialUse::not_cleared) {
            return core::Result<void>::failure(unsupported(
                "publicly redistributed character product lacks commercial-use "
                "clearance: " + identity));
        }
        if (rights.attribution_required && rights.attribution.empty()) {
            return core::Result<void>::failure(unsupported(
                "character product requires attribution but none was supplied: " +
                identity));
        }
    }
    if (grants.size() != product_source_digests.size()) {
        return core::Result<void>::failure(validation(
            "character license grant names a product that was not composed"));
    }
    return core::Result<void>::success();
}

core::Result<PreparedCharacterPackage> compose_prepared_character(
    const PreparedCharacterInput& input) {
    if (!safe_token(input.target_profile_id,
            character::CharacterPreparedManifest::kMaxProfileIdBytes) ||
        input.target_profile_digest.is_zero() ||
        !safe_token(input.producer_build, PreparedCharacterInput::kMaxBuildIdentityBytes) ||
        input.morphs.size() > PreparedCharacterInput::kMaxMorphs ||
        input.clips.size() > PreparedCharacterInput::kMaxClips ||
        input.secondary.size() > PreparedCharacterInput::kMaxSecondary ||
        input.required_capabilities.size() > PreparedCharacterInput::kMaxCapabilities) {
        return core::Result<PreparedCharacterPackage>::failure(invalid(
            "prepared character input identity, profile, or bounded collection is invalid"));
    }
    if (auto result = input.definition.validate(); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character definition"));
    }
    if (auto result = input.coordinate_conversion.validate(); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character coordinate conversion"));
    }
    character::ManifestCoordinate target_coordinate;
    target_coordinate.handedness = input.coordinate_conversion.target.handedness;
    target_coordinate.up = input.coordinate_conversion.target.up;
    target_coordinate.forward = input.coordinate_conversion.target.forward;
    target_coordinate.meters_per_unit = input.coordinate_conversion.target.meters_per_unit;
    if (auto result = check_vanta_coordinate(target_coordinate); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character target profile"));
    }

    // Source bindings: every digest must match the exact authoring snapshot.
    if (auto result = input.skeleton.validate(); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character skeleton descriptor"));
    }
    if (input.skeleton.graph_digest != input.definition.rig.graph_digest) {
        return core::Result<PreparedCharacterPackage>::failure(stale(
            "prepared character skeleton was lowered from a different rig graph"));
    }
    if (auto result = input.weights.binding.validate(); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character skin binding"));
    }
    if (input.weights.binding.graph_digest != input.skeleton.graph_digest) {
        return core::Result<PreparedCharacterPackage>::failure(stale(
            "prepared character weights were authored against a different joint graph"));
    }
    if (input.skeleton.skin_digest != digest_text(input.weights.binding.serialize())) {
        return core::Result<PreparedCharacterPackage>::failure(stale(
            "prepared character skeleton references a different skin binding"));
    }
    const auto mesh_ref = std::find_if(input.definition.mesh_refs.begin(),
        input.definition.mesh_refs.end(),
        [&input](const character::MeshAssetRef& ref) {
            return ref.asset_id == input.weights.binding.mesh_asset;
        });
    if (mesh_ref == input.definition.mesh_refs.end() ||
        mesh_ref->content_digest != input.weights.binding.mesh_digest) {
        return core::Result<PreparedCharacterPackage>::failure(stale(
            "prepared character weights were authored against a different mesh"));
    }
    if (auto result = input.weights.layer.validate(); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character skin weight layer"));
    }
    if (input.weights.layer.staleness != character::WeightStaleness::fresh ||
        input.weights.layer.joint_graph_digest != input.skeleton.graph_digest) {
        return core::Result<PreparedCharacterPackage>::failure(stale(
            "prepared character weight layer is stale and must be rebuilt first"));
    }

    // Deterministic ordering of the optional collections.
    if (!std::is_sorted(input.morphs.begin(), input.morphs.end(),
            [](const CharacterMorphProductInput& left, const CharacterMorphProductInput& right) {
                return left.target.id.value < right.target.id.value;
            }) ||
        std::adjacent_find(input.morphs.begin(), input.morphs.end(),
            [](const CharacterMorphProductInput& left, const CharacterMorphProductInput& right) {
                return left.target.id.value == right.target.id.value;
            }) != input.morphs.end()) {
        return core::Result<PreparedCharacterPackage>::failure(validation(
            "prepared character morphs must be sorted and unique by target id"));
    }
    if (!std::is_sorted(input.clips.begin(), input.clips.end(),
            [](const CharacterClipProductInput& left, const CharacterClipProductInput& right) {
                return left.source < right.source;
            }) ||
        std::adjacent_find(input.clips.begin(), input.clips.end(),
            [](const CharacterClipProductInput& left, const CharacterClipProductInput& right) {
                return !(left.source < right.source) && !(right.source < left.source);
            }) != input.clips.end()) {
        return core::Result<PreparedCharacterPackage>::failure(validation(
            "prepared character clips must be sorted and unique by clip reference"));
    }
    if (!std::is_sorted(input.secondary.begin(), input.secondary.end(),
            [](const CharacterSecondaryProductInput& left,
                const CharacterSecondaryProductInput& right) {
                return left.identity < right.identity;
            }) ||
        std::adjacent_find(input.secondary.begin(), input.secondary.end(),
            [](const CharacterSecondaryProductInput& left,
                const CharacterSecondaryProductInput& right) {
                return left.identity == right.identity;
            }) != input.secondary.end()) {
        return core::Result<PreparedCharacterPackage>::failure(validation(
            "prepared character secondary products must be sorted and unique by identity"));
    }
    if (!std::is_sorted(input.required_capabilities.begin(), input.required_capabilities.end()) ||
        std::adjacent_find(input.required_capabilities.begin(),
            input.required_capabilities.end()) != input.required_capabilities.end() ||
        !std::all_of(input.required_capabilities.begin(), input.required_capabilities.end(),
            [](const std::string& capability) {
                return safe_token(capability,
                    character::CharacterPreparedManifest::kMaxCapabilityBytes);
            })) {
        return core::Result<PreparedCharacterPackage>::failure(validation(
            "prepared character required capabilities must be sorted, unique, bounded tokens"));
    }

    // Build the product drafts. Payload bytes come from the canonical codecs
    // of the owning records wherever one exists (skeleton descriptor, skin
    // binding); opaque products (lowered weights, clips, secondary) are
    // digested exactly as supplied.
    std::vector<CharacterProductDraft> drafts;
    const auto skeleton_bytes = input.skeleton.serialize();
    drafts.push_back(CharacterProductDraft{
        "character.skeleton",
        std::string(kCharacterSkeletonProductKind),
        kCharacterProductSchemaMajor,
        manifest_path_for(kCharacterSkeletonProductKind),
        text_bytes(skeleton_bytes),
        "carto://character-producer/skeleton@v1",
        input.skeleton.graph_digest});

    const auto binding_bytes = input.weights.binding.serialize();
    drafts.push_back(CharacterProductDraft{
        "character.skin.weights8",
        std::string(kCharacterSkinProductKind),
        kCharacterProductSchemaMajor,
        "products/character.skin.weights8-v1.txt",
        text_bytes(binding_bytes),
        "carto://character-producer/skin-weights8@v1",
        input.weights.binding.mesh_digest});

    if (input.lowered_weights.has_value()) {
        const CharacterLoweredWeightsInput& lowered = input.lowered_weights.value();
        if (lowered.product_bytes.empty() ||
            lowered.product_bytes.size() > kMaxCharacterProductBytes ||
            !std::isfinite(lowered.certified_max_abs_error) ||
            lowered.certified_max_abs_error < 0.0) {
            return core::Result<PreparedCharacterPackage>::failure(unsupported(
                "prepared character lowered weights are not certified and cannot ship"));
        }
        drafts.push_back(CharacterProductDraft{
            "character.skin.weights4",
            std::string(kCharacterSkinProductKind),
            kCharacterProductSchemaMajor,
            "products/character.skin.weights4-v1.bin",
            lowered.product_bytes,
            "carto://character-producer/skin-weights4@v1",
            input.weights.binding.mesh_digest});
    }

    if (!input.morphs.empty()) {
        std::vector<std::uint8_t> morph_payload = text_bytes("CARTOGRAPHER_CHARACTER_MORPHS 1\n");
        for (const CharacterMorphProductInput& morph : input.morphs) {
            if (auto result = validate_morph_input(morph); !result) {
                return core::Result<PreparedCharacterPackage>::failure(
                    result.error().with_context("prepared character morph product"));
            }
            append_morph_target(morph_payload, morph);
        }
        const auto end_bytes = text_bytes("END\n");
        morph_payload.insert(morph_payload.end(), end_bytes.begin(), end_bytes.end());
        if (morph_payload.size() > kMaxCharacterProductBytes) {
            return core::Result<PreparedCharacterPackage>::failure(validation(
                "prepared character morph product exceeds its bounded size"));
        }
        drafts.push_back(CharacterProductDraft{
            "character.morphs",
            std::string(kCharacterMorphsProductKind),
            kCharacterProductSchemaMajor,
            manifest_path_for(kCharacterMorphsProductKind),
            std::move(morph_payload),
            "carto://character-producer/morphs@v1",
            input.morphs.front().target.source_mesh_digest});
    }

    for (const CharacterClipProductInput& clip : input.clips) {
        if (auto result = clip.source.validate(); !result) {
            return core::Result<PreparedCharacterPackage>::failure(
                result.error().with_context("prepared character clip reference"));
        }
        if (clip.clip_bytes.empty() || clip.clip_bytes.size() > kMaxCharacterProductBytes) {
            return core::Result<PreparedCharacterPackage>::failure(validation(
                "prepared character clip product is empty or exceeds its bounded size"));
        }
        const std::string identity =
            "character.motion.clip." + std::to_string(clip.source.clip.value);
        drafts.push_back(CharacterProductDraft{
            identity,
            std::string(kCharacterMotionProductKind),
            kCharacterProductSchemaMajor,
            "products/character.motion.clip-" + std::to_string(clip.source.clip.value) +
                "-v1.bin",
            clip.clip_bytes,
            "carto://character-producer/motion-clip@v1",
            clip.source.digest});
    }

    for (const CharacterSecondaryProductInput& secondary : input.secondary) {
        if (!safe_token(secondary.identity, CharacterSecondaryProductInput::kMaxIdentityBytes) ||
            !safe_token(secondary.descriptor_kind, CharacterSecondaryProductInput::kMaxKindBytes) ||
            secondary.descriptor_bytes.empty() ||
            secondary.descriptor_bytes.size() > kMaxCharacterProductBytes ||
            secondary.source_digest.is_zero()) {
            return core::Result<PreparedCharacterPackage>::failure(validation(
                "prepared character secondary product identity, kind, size, or source is invalid"));
        }
        drafts.push_back(CharacterProductDraft{
            secondary.identity,
            std::string(kCharacterSimulationProductKind),
            kCharacterProductSchemaMajor,
            "products/" + secondary.identity + "-v1.bin",
            secondary.descriptor_bytes,
            "carto://character-producer/secondary@v1",
            secondary.source_digest});
    }

    // Canonical order: manifest products sort by path, payloads in the same
    // order, one derivation receipt per product.
    std::sort(drafts.begin(), drafts.end(),
        [](const CharacterProductDraft& left, const CharacterProductDraft& right) {
            return left.relative_path < right.relative_path;
        });

    PreparedCharacterPackage package;
    package.manifest.schema_major = character::CharacterPreparedManifest::kSchemaMajor;
    package.manifest.schema_minor = character::CharacterPreparedManifest::kSchemaMinor;
    package.manifest.source.project_namespace = input.definition.project_namespace;
    package.manifest.source.revision = input.definition.source_revision.value();
    package.manifest.source.project_digest = input.definition.source_digest;
    package.manifest.coordinate = target_coordinate;
    package.manifest.character_id = input.definition.id;
    package.manifest.target_profile.id = input.target_profile_id;
    package.manifest.target_profile.digest = input.target_profile_digest;
    package.manifest.required_capabilities = input.required_capabilities;

    std::map<std::string, assets::Sha256Digest> source_digests;
    for (const CharacterProductDraft& draft : drafts) {
        const assets::Sha256Digest digest = digest_bytes(draft.bytes);
        package.manifest.products.push_back(character::ManifestProduct{
            draft.kind_contract,
            draft.relative_path,
            draft.schema,
            digest,
        });
        if (!source_digests.emplace(draft.identity, draft.source_digest).second) {
            return core::Result<PreparedCharacterPackage>::failure(validation(
                "prepared character product identities must be distinct"));
        }
        package.payloads.push_back(PreparedProductPayload{
            draft.identity, draft.bytes});
        character::CharacterProductReceipt receipt;
        receipt.producer_build = input.producer_build;
        receipt.source_namespace = input.definition.project_namespace;
        receipt.source_revision = core::Revision(input.definition.source_revision.value());
        receipt.source_digest = input.definition.source_digest;
        receipt.recipe_digest = digest_text(draft.recipe_identity);
        receipt.format_schema_major = kCharacterProductSchemaMajor;
        receipt.format_schema_minor = kCharacterProductSchemaMinor;
        receipt.backend_identity = "cpu-reference";
        receipt.precision_identity = "f64";
        receipt.output_digest = digest;
        package.receipts.push_back(std::move(receipt));
    }

    // Rights gate before anything is emitted: defective grants refuse the
    // whole package, never a partial one.
    if (auto result = check_character_product_licenses(source_digests, input.licenses);
        !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character license gate"));
    }
    if (auto result = package.manifest.validate(); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character manifest"));
    }
    std::map<std::string, assets::Sha256Digest> closure_digests;
    for (const character::ManifestProduct& product : package.manifest.products) {
        closure_digests.emplace(product.path, product.digest);
    }
    if (auto result = package.manifest.verify_closure(closure_digests); !result) {
        return core::Result<PreparedCharacterPackage>::failure(
            result.error().with_context("prepared character closure"));
    }
    for (const character::CharacterProductReceipt& receipt : package.receipts) {
        if (auto result = receipt.validate(); !result) {
            return core::Result<PreparedCharacterPackage>::failure(
                result.error().with_context("prepared character product receipt"));
        }
    }
    return core::Result<PreparedCharacterPackage>::success(std::move(package));
}

core::Result<character::CharacterPreparedManifest> verify_prepared_character(
    std::string_view manifest_bytes,
    const std::map<std::string, assets::Sha256Digest>& payload_digests) {
    if (manifest_bytes.empty() || manifest_bytes.size() > kMaxCharacterManifestBytes) {
        return core::Result<character::CharacterPreparedManifest>::failure(validation(
            "prepared character manifest is empty or exceeds its bounded size"));
    }
    const auto manifest =
        character::CharacterPreparedManifest::deserialize(manifest_bytes);
    if (!manifest) {
        return core::Result<character::CharacterPreparedManifest>::failure(
            manifest.error().with_context("prepared character manifest re-read"));
    }
    if (auto result = manifest.value().verify_closure(payload_digests); !result) {
        return core::Result<character::CharacterPreparedManifest>::failure(
            result.error().with_context("prepared character closure re-check"));
    }
    if (auto result = check_vanta_coordinate(manifest.value().coordinate); !result) {
        return core::Result<character::CharacterPreparedManifest>::failure(
            result.error().with_context("prepared character coordinate policy"));
    }
    return core::Result<character::CharacterPreparedManifest>::success(manifest.value());
}

core::Result<character::CharacterPreparedManifest> verify_prepared_character_package(
    const std::filesystem::path& package_dir) {
    if (package_dir.empty()) {
        return core::Result<character::CharacterPreparedManifest>::failure(invalid(
            "prepared character package directory must not be empty"));
    }
    std::error_code error;
    if (!std::filesystem::is_directory(package_dir, error) || error) {
        return core::Result<character::CharacterPreparedManifest>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "prepared character package directory is unavailable"));
    }
    const auto manifest_bytes = read_bounded_file(
        package_dir / "manifest.carto-character", kMaxCharacterManifestBytes,
        "manifest");
    if (!manifest_bytes) {
        return core::Result<character::CharacterPreparedManifest>::failure(
            manifest_bytes.error());
    }
    const std::string manifest_text(
        manifest_bytes.value().begin(), manifest_bytes.value().end());
    const auto manifest =
        character::CharacterPreparedManifest::deserialize(manifest_text);
    if (!manifest) {
        return core::Result<character::CharacterPreparedManifest>::failure(
            manifest.error().with_context("prepared character manifest re-read"));
    }

    // Enumerate the tree once: every declared file must exist, no extra
    // files or symlinks are admitted, nothing escapes the package root. The
    // prepared-scene envelope manifest is the CAS binding of the directory
    // and is not part of the character closure.
    constexpr std::string_view kEnvelopeFileName = "manifest.carto-prepared";
    std::set<std::string> expected_files{
        std::string(kCharacterManifestFileName), std::string(kEnvelopeFileName)};
    std::set<std::string> expected_directories;
    for (const character::ManifestProduct& product : manifest.value().products) {
        expected_files.insert(product.path);
        auto parent = std::filesystem::path(product.path).parent_path();
        while (!parent.empty()) {
            expected_directories.insert(parent.generic_string());
            parent = parent.parent_path();
        }
    }
    auto remaining_files = expected_files;
    std::map<std::string, assets::Sha256Digest> payload_digests;
    std::uintmax_t total_bytes = manifest_bytes.value().size();
    std::filesystem::recursive_directory_iterator cursor(
        package_dir, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
        return core::Result<character::CharacterPreparedManifest>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to enumerate prepared character package"));
    }
    while (cursor != end) {
        const auto entry_path = cursor->path();
        const auto status = cursor->symlink_status(error);
        if (error) {
            return core::Result<character::CharacterPreparedManifest>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to inspect prepared character package entry"));
        }
        if (std::filesystem::is_symlink(status)) {
            return core::Result<character::CharacterPreparedManifest>::failure(validation(
                "prepared character package contains a symbolic link"));
        }
        const auto relative =
            std::filesystem::relative(entry_path, package_dir, error).generic_string();
        if (error || relative.empty()) {
            return core::Result<character::CharacterPreparedManifest>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to resolve prepared character package entry"));
        }
        if (std::filesystem::is_directory(status)) {
            if (!expected_directories.contains(relative)) {
                return core::Result<character::CharacterPreparedManifest>::failure(validation(
                    "prepared character package contains an undeclared directory"));
            }
        } else if (!std::filesystem::is_regular_file(status) ||
                   remaining_files.erase(relative) != 1U) {
            return core::Result<character::CharacterPreparedManifest>::failure(validation(
                "prepared character package contains an undeclared file"));
        }
        if (std::filesystem::is_regular_file(status) && relative != kCharacterManifestFileName &&
            relative != kEnvelopeFileName) {
            const auto bytes = read_bounded_file(
                entry_path, kMaxCharacterProductBytes, "product");
            if (!bytes) {
                return core::Result<character::CharacterPreparedManifest>::failure(
                    bytes.error().with_context(relative));
            }
            if (total_bytes > kMaxCharacterPackageBytes - bytes.value().size()) {
                return core::Result<character::CharacterPreparedManifest>::failure(validation(
                    "prepared character package exceeds its aggregate byte bound"));
            }
            total_bytes += bytes.value().size();
            payload_digests.emplace(relative, digest_bytes(bytes.value()));
        }
        cursor.increment(error);
        if (error) {
            return core::Result<character::CharacterPreparedManifest>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to enumerate complete prepared character package"));
        }
    }
    if (!remaining_files.empty()) {
        return core::Result<character::CharacterPreparedManifest>::failure(validation(
            "prepared character package is missing a declared file"));
    }
    return verify_prepared_character(manifest_text, payload_digests);
}

} // namespace carto::production
