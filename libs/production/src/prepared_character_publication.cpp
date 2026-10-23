#include <carto/production/prepared_character_publication.hpp>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <span>
#include <set>
#include <utility>

namespace carto::production {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

PreparedProductKind prepared_kind_for(std::string_view kind_contract) {
    if (kind_contract == kCharacterSkeletonProductKind) {
        return PreparedProductKind::character_skeleton;
    }
    if (kind_contract == kCharacterSkinProductKind) {
        return PreparedProductKind::character_weights;
    }
    if (kind_contract == kCharacterMorphsProductKind) {
        return PreparedProductKind::character_morphs;
    }
    if (kind_contract == kCharacterMotionProductKind) {
        return PreparedProductKind::character_clips;
    }
    return PreparedProductKind::character_secondary;
}

// The character::Axis and PreparedAxis enumerators use different orders, so
// the mapping is explicit; a raw cast would silently swap axes.
PreparedAxis prepared_axis_for(character::Axis axis) noexcept {
    switch (axis) {
    case character::Axis::positive_x: return PreparedAxis::positive_x;
    case character::Axis::negative_x: return PreparedAxis::negative_x;
    case character::Axis::positive_y: return PreparedAxis::positive_y;
    case character::Axis::negative_y: return PreparedAxis::negative_y;
    case character::Axis::positive_z: return PreparedAxis::positive_z;
    case character::Axis::negative_z: return PreparedAxis::negative_z;
    }
    return PreparedAxis::positive_y;
}

// Mirrors the character manifest into the prepared-scene envelope shape so
// the existing immutable-revision CAS publisher governs character packages
// unchanged: same revisions/ store, same selector, same staged verify.
// Product identities carry over from the package payloads so the CAS
// payload-identity check sees the exact same set.
core::Result<PreparedSceneEnvelope> envelope_for_character_manifest(
    const character::CharacterPreparedManifest& manifest,
    const std::vector<PreparedProductPayload>& payloads) {
    // Map manifest path -> payload identity by digest, which is the
    // canonical binding between the manifest table and the payload set.
    std::map<std::string, std::string> identity_by_path;
    {
        std::map<assets::Sha256Digest, std::string> path_by_digest;
        for (const character::ManifestProduct& product : manifest.products) {
            path_by_digest.emplace(product.digest, product.path);
        }
        for (const PreparedProductPayload& payload : payloads) {
            // The staged manifest payload is bound to its own envelope
            // product by publish_prepared_character once the envelope is
            // built; it is not one of the manifest's declared products.
            if (payload.identity == kCharacterManifestProductIdentity) {
                continue;
            }
            const assets::Sha256Digest payload_digest = assets::sha256(payload.bytes);
            const auto path = path_by_digest.find(payload_digest);
            if (path == path_by_digest.end() ||
                !identity_by_path.emplace(path->second, payload.identity).second) {
                return core::Result<PreparedSceneEnvelope>::failure(validation(
                    "character package payload does not match a unique manifest product"));
            }
        }
    }
    PreparedSceneEnvelope envelope;
    envelope.source_namespace = manifest.source.project_namespace;
    envelope.source_revision = core::Revision(manifest.source.revision);
    envelope.source_digest = manifest.source.project_digest;
    envelope.profile_identity = manifest.target_profile.id;
    envelope.profile_digest = manifest.target_profile.digest;
    envelope.coordinates.handedness =
        manifest.coordinate.handedness == character::Handedness::right
        ? PreparedHandedness::right_handed
        : PreparedHandedness::left_handed;
    envelope.coordinates.up = prepared_axis_for(manifest.coordinate.up);
    envelope.coordinates.forward = prepared_axis_for(manifest.coordinate.forward);
    envelope.coordinates.meters_per_unit = manifest.coordinate.meters_per_unit;
    for (const character::ManifestProduct& product : manifest.products) {
        const auto identity = identity_by_path.find(product.path);
        if (identity == identity_by_path.end()) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "character package payload is missing for a manifest product"));
        }
        envelope.products.push_back(PreparedProductReference{
            identity->second,
            prepared_kind_for(product.kind),
            product.schema,
            product.digest,
            product.path,
        });
        envelope.dependencies.push_back(PreparedDependencyReference{
            identity->second,
            PreparedDependencyDisposition::included_product,
            true,
            core::Revision(manifest.source.revision),
            product.digest,
            product.path,
        });
    }
    for (const std::string& capability : manifest.required_capabilities) {
        envelope.capabilities.push_back(PreparedCapabilityResult{
            capability,
            PreparedCapabilityEvidence::derived_product,
            true,
            "character package required capability",
        });
    }
    // The prepared-scene envelope requires canonical identity ordering.
    std::sort(envelope.products.begin(), envelope.products.end(),
        [](const PreparedProductReference& left, const PreparedProductReference& right) {
            return left.identity < right.identity;
        });
    std::sort(envelope.dependencies.begin(), envelope.dependencies.end(),
        [](const PreparedDependencyReference& left, const PreparedDependencyReference& right) {
            return left.identity < right.identity;
        });
    return core::Result<PreparedSceneEnvelope>::success(std::move(envelope));
}

} // namespace

core::Result<void> CharacterPackagePublicationReceipt::validate() const {
    if (manifest_digest.is_zero() || selected_digest.is_zero() ||
        closure_digest.is_zero() ||
        source_revision.value() == 0U || revision_path.empty()) {
        return core::Result<void>::failure(invalid(
            "character package publication receipt digest, revision, or path is invalid"));
    }
    return core::Result<void>::success();
}

std::string CharacterPackagePublicationReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_CHARACTER_PACKAGE_PUBLICATION 1\n";
    output << "MANIFEST " << manifest_digest.hex() << '\n';
    output << "SELECTED " << selected_digest.hex() << '\n';
    output << "CLOSURE " << closure_digest.hex() << '\n';
    output << "PREVIOUS " <<
        (previous_selected_digest.has_value() ? previous_selected_digest->hex() : std::string{"-"})
        << '\n';
    output << "SOURCE_REVISION " << source_revision.value() << '\n';
    output << "REVISION_PATH " << std::quoted(revision_path.generic_string()) << '\n';
    output << "REUSED " << (reused_immutable_revision ? 1U : 0U) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CharacterPackagePublicationReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical));
}

core::Result<void> CharacterPublicationRollbackReceipt::validate() const {
    if (attempted_selected_digest.is_zero() ||
        !safe_token(failed_stage, kMaxStageBytes) ||
        !safe_token(detail, kMaxDetailBytes, true)) {
        return core::Result<void>::failure(invalid(
            "character rollback receipt digest, stage, or detail is invalid"));
    }
    return core::Result<void>::success();
}

std::string CharacterPublicationRollbackReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_CHARACTER_ROLLBACK 1\n";
    output << "ATTEMPTED " << attempted_selected_digest.hex() << '\n';
    output << "PRESERVED " <<
        (preserved_selected_digest.has_value() ? preserved_selected_digest->hex() : std::string{"-"})
        << '\n';
    output << "STAGE " << std::quoted(failed_stage) << '\n';
    output << "DETAIL " << std::quoted(detail) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CharacterPublicationRollbackReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical));
}

core::Result<void> CharacterPackageMigrationReceipt::validate() const {
    if (!safe_token(package_identity, kMaxIdentityBytes) ||
        expected_basis_revision.value() == 0U || migrated_revision.value() == 0U ||
        from_schema_major == 0U || to_schema_major == 0U ||
        backup_digest.is_zero() || migrated_manifest_digest.is_zero() ||
        loss_report.size() > kMaxLossEntries ||
        !std::is_sorted(loss_report.begin(), loss_report.end()) ||
        std::adjacent_find(loss_report.begin(), loss_report.end()) != loss_report.end() ||
        !std::all_of(loss_report.begin(), loss_report.end(), [](const std::string& entry) {
            return safe_token(entry, kMaxLossEntryBytes);
        })) {
        return core::Result<void>::failure(invalid(
            "character package migration receipt identity, revision, schema, digest, "
            "or loss report is invalid"));
    }
    return core::Result<void>::success();
}

std::string CharacterPackageMigrationReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_CHARACTER_PACKAGE_MIGRATION 1\n";
    output << "PACKAGE " << std::quoted(package_identity) << '\n';
    output << "BASIS_REVISION " << expected_basis_revision.value() << '\n';
    output << "MIGRATED_REVISION " << migrated_revision.value() << '\n';
    output << "FROM_SCHEMA " << from_schema_major << ' ' << from_schema_minor << '\n';
    output << "TO_SCHEMA " << to_schema_major << ' ' << to_schema_minor << '\n';
    output << "BACKUP " << backup_digest.hex() << '\n';
    output << "MIGRATED " << migrated_manifest_digest.hex() << '\n';
    output << "ROLLBACK_PATH " << (rollback_path_available ? 1U : 0U) << '\n';
    output << "LOSS " << loss_report.size() << '\n';
    for (const std::string& entry : loss_report) {
        output << "LOSS_ENTRY " << std::quoted(entry) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CharacterPackageMigrationReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical));
}

core::Result<CharacterPackagePublicationReceipt> publish_prepared_character(
    const std::filesystem::path& output_root,
    const PreparedCharacterPackage& package,
    std::optional<assets::Sha256Digest> expected_selected_digest) {
    const auto manifest_digest = package.manifest.canonical_digest();
    if (!manifest_digest) {
        return core::Result<CharacterPackagePublicationReceipt>::failure(
            manifest_digest.error().with_context("character package manifest"));
    }
    if (auto result = package.manifest.validate(); !result) {
        return core::Result<CharacterPackagePublicationReceipt>::failure(
            result.error().with_context("character package manifest"));
    }
    // Stage the character manifest itself as a product so the immutable
    // revision carries the exact closure envelope its digests bind; recovery
    // re-reads it instead of inferring validity from a filename.
    std::vector<PreparedProductPayload> staged_payloads = package.payloads;
    {
        const std::string manifest_text = package.manifest.serialize();
        staged_payloads.push_back(PreparedProductPayload{
            std::string(kCharacterManifestProductIdentity),
            std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(manifest_text.data()),
                reinterpret_cast<const std::uint8_t*>(manifest_text.data()) +
                    manifest_text.size())});
    }
    auto envelope = envelope_for_character_manifest(package.manifest, staged_payloads);
    if (!envelope) {
        return core::Result<CharacterPackagePublicationReceipt>::failure(envelope.error());
    }
    envelope.value().products.push_back(PreparedProductReference{
        std::string(kCharacterManifestProductIdentity),
        PreparedProductKind::character_manifest,
        package.manifest.schema_major,
        manifest_digest.value(),
        std::string(kCharacterManifestFileName),
    });
    envelope.value().dependencies.push_back(PreparedDependencyReference{
        std::string(kCharacterManifestProductIdentity),
        PreparedDependencyDisposition::included_product,
        true,
        core::Revision(package.manifest.source.revision),
        manifest_digest.value(),
        std::string(kCharacterManifestFileName),
    });
    // Identity ordering is a hard envelope invariant; the manifest product
    // was appended after the builder's sort.
    std::sort(envelope.value().products.begin(), envelope.value().products.end(),
        [](const PreparedProductReference& left, const PreparedProductReference& right) {
            return left.identity < right.identity;
        });
    std::sort(envelope.value().dependencies.begin(), envelope.value().dependencies.end(),
        [](const PreparedDependencyReference& left, const PreparedDependencyReference& right) {
            return left.identity < right.identity;
        });
    const auto selected_digest = envelope.value().canonical_digest();
    if (!selected_digest) {
        return core::Result<CharacterPackagePublicationReceipt>::failure(
            selected_digest.error().with_context("character package selector envelope"));
    }
    const auto published = publish_prepared_scene(
        output_root, envelope.value(), staged_payloads, expected_selected_digest);
    if (!published) {
        return core::Result<CharacterPackagePublicationReceipt>::failure(
            published.error().with_context("character package publication"));
    }
    return core::Result<CharacterPackagePublicationReceipt>::success(
        CharacterPackagePublicationReceipt{
            manifest_digest.value(),
            selected_digest.value(),
            published.value().dependency_closure_digest,
            published.value().previous_selected_digest,
            published.value().source_revision,
            published.value().revision_path,
            published.value().reused_immutable_revision,
        });
}

core::Result<CharacterPublicationRollbackReceipt> capture_character_rollback_receipt(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& attempted_selected_digest,
    std::string failed_stage,
    std::string detail) {
    const auto selected = read_selected_prepared_scene(output_root);
    if (!selected) {
        return core::Result<CharacterPublicationRollbackReceipt>::failure(
            selected.error().with_context("character rollback selection re-read"));
    }
    CharacterPublicationRollbackReceipt receipt;
    receipt.attempted_selected_digest = attempted_selected_digest;
    if (selected.value().has_value()) {
        receipt.preserved_selected_digest = selected.value()->prepared_scene_digest;
    }
    receipt.failed_stage = std::move(failed_stage);
    receipt.detail = std::move(detail);
    if (auto result = receipt.validate(); !result) {
        return core::Result<CharacterPublicationRollbackReceipt>::failure(result.error());
    }
    return core::Result<CharacterPublicationRollbackReceipt>::success(std::move(receipt));
}

core::Result<CharacterPackageMigrationReceipt> make_character_migration_receipt(
    std::string package_identity,
    core::Revision expected_basis_revision,
    core::Revision migrated_revision,
    std::uint32_t from_schema_major,
    std::uint32_t from_schema_minor,
    std::uint32_t to_schema_major,
    std::uint32_t to_schema_minor,
    std::span<const std::uint8_t> preserved_original_bytes,
    const character::CharacterPreparedManifest& migrated_manifest,
    bool rollback_path_available,
    std::vector<std::string> loss_report) {
    if (preserved_original_bytes.empty()) {
        return core::Result<CharacterPackageMigrationReceipt>::failure(invalid(
            "character package migration requires the preserved original bytes"));
    }
    const auto migrated_digest = migrated_manifest.canonical_digest();
    if (!migrated_digest) {
        return core::Result<CharacterPackageMigrationReceipt>::failure(
            migrated_digest.error().with_context("migrated character manifest"));
    }
    std::sort(loss_report.begin(), loss_report.end());
    if (std::adjacent_find(loss_report.begin(), loss_report.end()) != loss_report.end()) {
        return core::Result<CharacterPackageMigrationReceipt>::failure(validation(
            "character package migration loss report must be unique"));
    }
    CharacterPackageMigrationReceipt receipt;
    receipt.package_identity = std::move(package_identity);
    receipt.expected_basis_revision = expected_basis_revision;
    receipt.migrated_revision = migrated_revision;
    receipt.from_schema_major = from_schema_major;
    receipt.from_schema_minor = from_schema_minor;
    receipt.to_schema_major = to_schema_major;
    receipt.to_schema_minor = to_schema_minor;
    receipt.backup_digest = digest_text(std::string_view{
        reinterpret_cast<const char*>(preserved_original_bytes.data()),
        preserved_original_bytes.size()});
    receipt.migrated_manifest_digest = migrated_digest.value();
    receipt.rollback_path_available = rollback_path_available;
    receipt.loss_report = std::move(loss_report);
    if (auto result = receipt.validate(); !result) {
        return core::Result<CharacterPackageMigrationReceipt>::failure(result.error());
    }
    return core::Result<CharacterPackageMigrationReceipt>::success(std::move(receipt));
}

} // namespace carto::production
