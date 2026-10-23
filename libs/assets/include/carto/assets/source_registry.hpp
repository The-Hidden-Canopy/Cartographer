#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/botanical/source.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::assets {

enum class SourceAssetKind : std::uint8_t {
    mesh = 0U,
    material = 1U,
    texture = 2U,
    image = 3U,
    audio = 4U,
    terrain = 5U,
    other = 6U,
};

enum class SourceAssetDomain : std::uint8_t {
    geometry = 0U,
    hierarchy = 1U,
    material = 2U,
};

enum class SourceReimportChange : std::uint8_t {
    added = 0U,
    unchanged = 1U,
    metadata_only = 2U,
    geometry_changed = 3U,
    hierarchy_changed = 4U,
    material_changed = 5U,
    multiple_domains_changed = 6U,
    source_changed_unclassified = 7U,
    removed = 8U,
    identity_conflict = 9U,
};

struct SourceImportOptions final {
    double meters_per_source_unit = 1.0;
    std::uint8_t up_axis = 1U; // 0=X, 1=Y, 2=Z
    bool right_handed = true;
    bool triangulate = false;
    bool import_hierarchy = true;
    bool import_materials = true;
    bool import_normals = true;
    bool weld_vertices = false;
    double weld_tolerance_meters = 1.0e-5;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
    [[nodiscard]] bool operator==(const SourceImportOptions&) const noexcept = default;
};

struct SourceDomainDigest final {
    SourceAssetDomain domain = SourceAssetDomain::geometry;
    assets::Sha256Digest digest;

    [[nodiscard]] bool operator==(const SourceDomainDigest&) const noexcept = default;
};

// Local provenance for an explicitly submitted private botanical job. The
// serialized record retains only the canonical source and digest bindings; it
// deliberately omits the service URL, credentials, organization, raw job ID,
// and private package identity.
struct BotanicalGenerationReceipt final {
    static constexpr std::size_t kMaxSourceSnapshotBytes =
        botanical::Source::kMaxSerializedBytes;

    std::string source_snapshot;
    Sha256Digest job_identity_digest;
    Sha256Digest input_artifact_digest;
    Sha256Digest output_artifact_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<BotanicalGenerationReceipt> deserialize(
        std::string_view text);
};

// Metadata is supplied by an importer after it has independently verified the
// source bytes and decoded product. This record never stores absolute paths,
// decoder state, or external runtime objects.
struct SourceImportRecord final {
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxRelativePathBytes = 2'048U;
    static constexpr std::size_t kMaxImporterIdentityBytes = 128U;
    static constexpr std::size_t kMaxDomainDigests = 3U;

    std::string identity;
    std::string relative_source_path;
    core::Revision source_generation;
    SourceAssetKind kind = SourceAssetKind::other;
    assets::Sha256Digest source_digest;
    std::string importer_identity;
    std::uint32_t importer_version = 1U;
    SourceImportOptions options;
    std::string artifact_identity;
    assets::Sha256Digest artifact_digest;
    std::vector<SourceDomainDigest> domain_digests;
    std::optional<BotanicalGenerationReceipt> botanical_generation;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SourceImportRecord> deserialize(
        std::string_view text);
};

class ProjectAssetRegistry final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxRecords = 65'536U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxAggregateRecordBytes = 24U * 1024U * 1024U;

    [[nodiscard]] const std::map<std::string, SourceImportRecord>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] const std::map<std::string, core::Revision>& lineage() const noexcept {
        return lineage_;
    }
    [[nodiscard]] core::Result<void> set_record(SourceImportRecord record);
    [[nodiscard]] core::Result<void> erase_record(
        std::string_view identity,
        core::Revision expected_generation);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ProjectAssetRegistry> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<std::string, SourceImportRecord> records_;
    // Retain the latest generation after removal; a recreated source must
    // advance rather than making an old receipt current again.
    std::map<std::string, core::Revision> lineage_;
};

struct SourceReimportPlanEntry final {
    std::string identity;
    SourceReimportChange change = SourceReimportChange::unchanged;
    assets::Sha256Digest previous_digest;
    std::optional<SourceImportRecord> incoming;
};

// Advisory plan only. Constructing it never changes the registry or adopts an
// imported product; a separate review/transaction step is required.
struct SourceReimportPlan final {
    assets::Sha256Digest base_registry_digest;
    assets::Sha256Digest incoming_set_digest;
    std::vector<SourceReimportPlanEntry> entries;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

[[nodiscard]] core::Result<SourceReimportPlan> plan_source_reimport(
    const ProjectAssetRegistry& current,
    std::vector<SourceImportRecord> incoming);

// Revalidates a reviewed plan against its exact base snapshot and returns a
// new registry. No current registry is mutated if the plan is stale or invalid.
[[nodiscard]] core::Result<ProjectAssetRegistry> apply_source_reimport_plan(
    const ProjectAssetRegistry& current,
    const SourceReimportPlan& plan);

enum class SourceRelinkDisposition : std::uint8_t {
    moved = 0U,
};

// Advisory review for rebinding a record whose on-disk source moved. The
// staged snapshot must be digest-identical to the bound source; changed
// content is refused here and must use the source reimport flow instead.
struct SourceRelinkPlan final {
    assets::Sha256Digest base_registry_digest;
    std::string identity;
    core::Revision expected_generation;
    std::string staged_relative_path;
    assets::Sha256Digest staged_digest;
    SourceRelinkDisposition disposition = SourceRelinkDisposition::moved;
    SourceImportRecord incoming;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

[[nodiscard]] core::Result<SourceRelinkPlan> plan_source_relink(
    const ProjectAssetRegistry& current,
    std::string_view identity,
    core::Revision expected_generation,
    std::string staged_relative_path,
    assets::Sha256Digest staged_digest);

// Revalidates a reviewed relink against its exact base snapshot and returns a
// new registry. No current registry is mutated if the plan is stale or forged.
[[nodiscard]] core::Result<ProjectAssetRegistry> apply_source_relink_plan(
    const ProjectAssetRegistry& current,
    const SourceRelinkPlan& plan);

} // namespace carto::assets
