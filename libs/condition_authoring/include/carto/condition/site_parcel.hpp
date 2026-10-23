#pragma once

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::condition {

using SiteParcelModelId = std::uint64_t;
using SiteParcelId = std::uint64_t;
using SiteParcelFrontageId = std::uint64_t;
using SiteParcelAccessId = std::uint64_t;
using SiteParcelGradeRelationId = std::uint64_t;
using SiteParcelDrainageRelationId = std::uint64_t;

enum class SiteParcelSourceKind : std::uint8_t {
    parcel_boundary,
    road,
    grade_surface,
    drainage_receiver,
};

enum class SiteParcelFrontageClaim : std::uint8_t {
    unresolved,
    authored,
};

enum class SiteParcelAccessKind : std::uint8_t {
    vehicle,
    pedestrian,
    service,
    emergency,
};

enum class SiteParcelAccessClaim : std::uint8_t {
    unresolved,
    proposed,
    author_claims_available,
    author_claims_restricted,
    author_claims_closed,
};

enum class SiteParcelGradeClaim : std::uint8_t {
    unresolved,
    authored_equal,
    author_claims_first_higher,
    author_claims_second_higher,
};

enum class SiteParcelDrainageClaim : std::uint8_t {
    unresolved,
    intended_to_receiver,
};

enum class SiteParcelRecordKind : std::uint8_t {
    parcel,
    frontage,
    access,
    grade_relation,
    drainage_relation,
};

enum class SiteParcelDiagnosticCode : std::uint8_t {
    missing_boundary,
    missing_frontage,
    missing_access,
    unresolved_frontage,
    frontage_boundary_mismatch,
    unresolved_access,
    missing_frontage_reference,
    unresolved_grade,
    missing_grade_source,
    unresolved_drainage,
    missing_drainage_receiver,
    missing_source_revision,
    stale_source_revision,
};

struct SiteParcelSourceRevision {
    SiteParcelSourceKind kind = SiteParcelSourceKind::parcel_boundary;
    std::string identity;
    std::uint64_t revision = 0U;
    friend bool operator==(const SiteParcelSourceRevision&,
        const SiteParcelSourceRevision&) = default;
};

// A source reference binds an authored relationship to one exact source
// revision. It is never silently upgraded to a newer source revision.
struct SiteParcelSourceReference {
    SiteParcelSourceKind kind = SiteParcelSourceKind::parcel_boundary;
    std::string identity;
    std::uint64_t revision = 0U;
    friend bool operator==(const SiteParcelSourceReference&,
        const SiteParcelSourceReference&) = default;
};

struct SiteParcelRecord {
    SiteParcelId id = 0U;
    std::optional<SiteParcelSourceReference> boundary;
    friend bool operator==(const SiteParcelRecord&, const SiteParcelRecord&) = default;
};

struct SiteParcelFrontage {
    SiteParcelFrontageId id = 0U;
    SiteParcelId parcel_id = 0U;
    std::optional<SiteParcelSourceReference> boundary;
    std::optional<SiteParcelSourceReference> road;
    SiteParcelFrontageClaim claim = SiteParcelFrontageClaim::unresolved;
    friend bool operator==(const SiteParcelFrontage&, const SiteParcelFrontage&) = default;
};

struct SiteParcelAccess {
    SiteParcelAccessId id = 0U;
    SiteParcelId parcel_id = 0U;
    // Missing or stale frontage references are reported by inspect(); the
    // model does not invent an access path to repair them.
    std::optional<SiteParcelFrontageId> frontage_id;
    SiteParcelAccessKind kind = SiteParcelAccessKind::vehicle;
    SiteParcelAccessClaim claim = SiteParcelAccessClaim::unresolved;
    friend bool operator==(const SiteParcelAccess&, const SiteParcelAccess&) = default;
};

struct SiteParcelGradeRelation {
    SiteParcelGradeRelationId id = 0U;
    SiteParcelId first_parcel_id = 0U;
    SiteParcelId second_parcel_id = 0U;
    std::optional<SiteParcelSourceReference> first_surface;
    std::optional<SiteParcelSourceReference> second_surface;
    SiteParcelGradeClaim claim = SiteParcelGradeClaim::unresolved;
    friend bool operator==(const SiteParcelGradeRelation&,
        const SiteParcelGradeRelation&) = default;
};

struct SiteParcelDrainageRelation {
    SiteParcelDrainageRelationId id = 0U;
    SiteParcelId parcel_id = 0U;
    std::optional<SiteParcelSourceReference> receiver;
    SiteParcelDrainageClaim claim = SiteParcelDrainageClaim::unresolved;
    friend bool operator==(const SiteParcelDrainageRelation&,
        const SiteParcelDrainageRelation&) = default;
};

struct SiteParcelState {
    // These are caller-supplied source revisions, not a live survey or an
    // inferred geometry query. Source identities must be unique in this list.
    std::vector<SiteParcelSourceRevision> current_sources;
    std::vector<SiteParcelRecord> parcels;
    std::vector<SiteParcelFrontage> frontages;
    std::vector<SiteParcelAccess> accesses;
    std::vector<SiteParcelGradeRelation> grade_relations;
    std::vector<SiteParcelDrainageRelation> drainage_relations;
    friend bool operator==(const SiteParcelState&, const SiteParcelState&) = default;
};

struct SiteParcelDiagnostic {
    SiteParcelDiagnosticCode code = SiteParcelDiagnosticCode::missing_boundary;
    SiteParcelRecordKind record_kind = SiteParcelRecordKind::parcel;
    std::uint64_t record_id = 0U;
    std::string source_identity;
    std::uint64_t expected_source_revision = 0U;
    std::uint64_t current_source_revision = 0U;
    friend bool operator==(const SiteParcelDiagnostic&,
        const SiteParcelDiagnostic&) = default;
};

struct SiteParcelReport {
    SiteParcelModelId model_id = 0U;
    std::uint64_t revision = 0U;
    std::vector<SiteParcelDiagnostic> diagnostics;

    [[nodiscard]] bool needs_review() const noexcept {
        return !diagnostics.empty();
    }
};

struct SiteParcelEditReceipt {
    SiteParcelModelId model_id = 0U;
    std::uint64_t revision_before = 0U;
    std::uint64_t revision_after = 0U;
    std::vector<std::string> changed_source_identities;
    std::vector<SiteParcelId> changed_parcel_ids;
    std::vector<SiteParcelFrontageId> changed_frontage_ids;
    std::vector<SiteParcelAccessId> changed_access_ids;
    std::vector<SiteParcelGradeRelationId> changed_grade_relation_ids;
    std::vector<SiteParcelDrainageRelationId> changed_drainage_relation_ids;
};

// Revisioned, provider-neutral site/parcel relationships. Geometry,
// ownership, surveyed boundaries, access rights, grade measurements, and
// drainage engineering are authored claims and are never derived here.
class SiteParcelModel final {
public:
    static constexpr std::size_t kMaxRecordsPerKind = 65'536U;
    static constexpr std::size_t kMaxSourceIdentityBytes = 128U;

    [[nodiscard]] static core::Result<SiteParcelModel> create(
        SiteParcelModelId id, SiteParcelState state = {});

    [[nodiscard]] SiteParcelModelId id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const SiteParcelState& state() const noexcept { return state_; }

    // Replaces one complete authored snapshot atomically. Callers must provide
    // the exact current revision; edits never merge or discard contested data.
    [[nodiscard]] core::Result<SiteParcelEditReceipt> apply(
        std::uint64_t expected_revision, SiteParcelState replacement);
    [[nodiscard]] core::Result<SiteParcelReport> inspect() const;

private:
    SiteParcelModel(SiteParcelModelId id, SiteParcelState state) noexcept;

    SiteParcelModelId id_ = 0U;
    std::uint64_t revision_ = 1U;
    SiteParcelState state_;
};

} // namespace carto::condition
