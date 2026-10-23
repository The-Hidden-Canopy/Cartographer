#include <carto/condition/site_parcel.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool nonblank(const std::string& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' &&
            ch != '\v' && ch != '\f';
    });
}

bool valid_source_kind(SiteParcelSourceKind kind) noexcept {
    switch (kind) {
    case SiteParcelSourceKind::parcel_boundary:
    case SiteParcelSourceKind::road:
    case SiteParcelSourceKind::grade_surface:
    case SiteParcelSourceKind::drainage_receiver:
        return true;
    }
    return false;
}

bool valid_frontage_claim(SiteParcelFrontageClaim claim) noexcept {
    return claim == SiteParcelFrontageClaim::unresolved ||
        claim == SiteParcelFrontageClaim::authored;
}

bool valid_access_kind(SiteParcelAccessKind kind) noexcept {
    switch (kind) {
    case SiteParcelAccessKind::vehicle:
    case SiteParcelAccessKind::pedestrian:
    case SiteParcelAccessKind::service:
    case SiteParcelAccessKind::emergency:
        return true;
    }
    return false;
}

bool valid_access_claim(SiteParcelAccessClaim claim) noexcept {
    switch (claim) {
    case SiteParcelAccessClaim::unresolved:
    case SiteParcelAccessClaim::proposed:
    case SiteParcelAccessClaim::author_claims_available:
    case SiteParcelAccessClaim::author_claims_restricted:
    case SiteParcelAccessClaim::author_claims_closed:
        return true;
    }
    return false;
}

bool valid_grade_claim(SiteParcelGradeClaim claim) noexcept {
    switch (claim) {
    case SiteParcelGradeClaim::unresolved:
    case SiteParcelGradeClaim::authored_equal:
    case SiteParcelGradeClaim::author_claims_first_higher:
    case SiteParcelGradeClaim::author_claims_second_higher:
        return true;
    }
    return false;
}

bool valid_drainage_claim(SiteParcelDrainageClaim claim) noexcept {
    return claim == SiteParcelDrainageClaim::unresolved ||
        claim == SiteParcelDrainageClaim::intended_to_receiver;
}

bool valid_reference(const SiteParcelSourceReference& reference) noexcept {
    return valid_source_kind(reference.kind) && nonblank(reference.identity) &&
        reference.identity.size() <= SiteParcelModel::kMaxSourceIdentityBytes &&
        reference.revision != 0U;
}

void canonicalize(SiteParcelState& state) {
    std::sort(state.current_sources.begin(), state.current_sources.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.kind, left.identity) <
                std::tie(right.kind, right.identity);
        });
    std::sort(state.parcels.begin(), state.parcels.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(state.frontages.begin(), state.frontages.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(state.accesses.begin(), state.accesses.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(state.grade_relations.begin(), state.grade_relations.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(state.drainage_relations.begin(), state.drainage_relations.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
}

template <typename T, typename Id>
bool unique_nonzero_ids(const std::vector<T>& values, Id id_of) {
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(values.size());
    for (const auto& value : values) {
        if (id_of(value) == 0U || !ids.insert(id_of(value)).second) return false;
    }
    return true;
}

const SiteParcelSourceRevision* find_source(
    const SiteParcelState& state, SiteParcelSourceKind kind, const std::string& identity) {
    const auto found = std::lower_bound(state.current_sources.begin(),
        state.current_sources.end(), std::pair{kind, identity},
        [](const SiteParcelSourceRevision& source, const auto& key) {
            return std::tuple{source.kind, source.identity} <
                std::tuple{key.first, key.second};
        });
    if (found == state.current_sources.end() || found->kind != kind ||
        found->identity != identity) {
        return nullptr;
    }
    return &*found;
}

core::Result<void> validate_state(const SiteParcelState& state) {
    const auto within_limit = [](std::size_t count) {
        return count <= SiteParcelModel::kMaxRecordsPerKind;
    };
    if (!within_limit(state.current_sources.size()) || !within_limit(state.parcels.size()) ||
        !within_limit(state.frontages.size()) || !within_limit(state.accesses.size()) ||
        !within_limit(state.grade_relations.size()) ||
        !within_limit(state.drainage_relations.size())) {
        return core::Result<void>::failure(invalid("site/parcel record limit exceeded"));
    }

    std::unordered_set<std::string> source_identities;
    source_identities.reserve(state.current_sources.size());
    for (const auto& source : state.current_sources) {
        if (!valid_source_kind(source.kind) || !nonblank(source.identity) ||
            source.identity.size() > SiteParcelModel::kMaxSourceIdentityBytes ||
            source.revision == 0U || !source_identities.insert(source.identity).second) {
            return core::Result<void>::failure(invalid(
                "site/parcel source identity, kind, revision, or uniqueness is invalid"));
        }
    }

    if (!unique_nonzero_ids(state.parcels,
            [](const SiteParcelRecord& value) { return value.id; }) ||
        !unique_nonzero_ids(state.frontages,
            [](const SiteParcelFrontage& value) { return value.id; }) ||
        !unique_nonzero_ids(state.accesses,
            [](const SiteParcelAccess& value) { return value.id; }) ||
        !unique_nonzero_ids(state.grade_relations,
            [](const SiteParcelGradeRelation& value) { return value.id; }) ||
        !unique_nonzero_ids(state.drainage_relations,
            [](const SiteParcelDrainageRelation& value) { return value.id; })) {
        return core::Result<void>::failure(invalid(
            "site/parcel record IDs must be non-zero and unique within each record kind"));
    }

    const auto valid_optional_reference = [](
        const std::optional<SiteParcelSourceReference>& reference) {
        return !reference.has_value() || valid_reference(*reference);
    };
    std::unordered_set<SiteParcelId> parcel_ids;
    parcel_ids.reserve(state.parcels.size());
    for (const auto& parcel : state.parcels) {
        if (!parcel_ids.insert(parcel.id).second ||
            !valid_optional_reference(parcel.boundary) ||
            (parcel.boundary && parcel.boundary->kind !=
                SiteParcelSourceKind::parcel_boundary)) {
            return core::Result<void>::failure(invalid(
                "site/parcel boundary identity or source reference is invalid"));
        }
    }

    std::unordered_set<SiteParcelFrontageId> frontage_ids;
    frontage_ids.reserve(state.frontages.size());
    for (const auto& frontage : state.frontages) {
        if (!frontage_ids.insert(frontage.id).second ||
            !parcel_ids.contains(frontage.parcel_id) ||
            !valid_optional_reference(frontage.boundary) ||
            !valid_optional_reference(frontage.road) ||
            (frontage.boundary && frontage.boundary->kind !=
                SiteParcelSourceKind::parcel_boundary) ||
            (frontage.road && frontage.road->kind != SiteParcelSourceKind::road) ||
            !valid_frontage_claim(frontage.claim)) {
            return core::Result<void>::failure(invalid(
                "site/parcel frontage identity, endpoint, or source reference is invalid"));
        }
    }

    for (const auto& access : state.accesses) {
        if (!parcel_ids.contains(access.parcel_id) ||
            (access.frontage_id && *access.frontage_id == 0U) ||
            !valid_access_kind(access.kind) || !valid_access_claim(access.claim)) {
            return core::Result<void>::failure(invalid(
                "site/parcel access identity, endpoint, or claim is invalid"));
        }
    }

    for (const auto& relation : state.grade_relations) {
        if (relation.first_parcel_id == relation.second_parcel_id ||
            !parcel_ids.contains(relation.first_parcel_id) ||
            !parcel_ids.contains(relation.second_parcel_id) ||
            !valid_optional_reference(relation.first_surface) ||
            !valid_optional_reference(relation.second_surface) ||
            (relation.first_surface && relation.first_surface->kind !=
                SiteParcelSourceKind::parcel_boundary && relation.first_surface->kind !=
                SiteParcelSourceKind::grade_surface) ||
            (relation.second_surface && relation.second_surface->kind !=
                SiteParcelSourceKind::parcel_boundary && relation.second_surface->kind !=
                SiteParcelSourceKind::grade_surface) || !valid_grade_claim(relation.claim)) {
            return core::Result<void>::failure(invalid(
                "site/parcel grade relation endpoints or source references are invalid"));
        }
    }

    for (const auto& relation : state.drainage_relations) {
        if (!parcel_ids.contains(relation.parcel_id) ||
            !valid_optional_reference(relation.receiver) ||
            (relation.receiver && relation.receiver->kind !=
                SiteParcelSourceKind::drainage_receiver && relation.receiver->kind !=
                SiteParcelSourceKind::road && relation.receiver->kind !=
                SiteParcelSourceKind::parcel_boundary) ||
            !valid_drainage_claim(relation.claim)) {
            return core::Result<void>::failure(invalid(
                "site/parcel drainage relation endpoint or receiver is invalid"));
        }
    }
    return core::Result<void>::success();
}

template <typename T, typename Id>
std::vector<std::uint64_t> changed_ids(
    const std::vector<T>& before, const std::vector<T>& after, Id id_of) {
    std::unordered_map<std::uint64_t, const T*> before_by_id;
    std::unordered_map<std::uint64_t, const T*> after_by_id;
    before_by_id.reserve(before.size());
    after_by_id.reserve(after.size());
    for (const auto& record : before) before_by_id.emplace(id_of(record), &record);
    for (const auto& record : after) after_by_id.emplace(id_of(record), &record);
    std::set<std::uint64_t> all_ids;
    for (const auto& [id, record] : before_by_id) {
        static_cast<void>(record);
        all_ids.insert(id);
    }
    for (const auto& [id, record] : after_by_id) {
        static_cast<void>(record);
        all_ids.insert(id);
    }
    std::vector<std::uint64_t> result;
    for (const auto id : all_ids) {
        const auto previous = before_by_id.find(id);
        const auto replacement = after_by_id.find(id);
        if (previous == before_by_id.end() || replacement == after_by_id.end() ||
            *previous->second != *replacement->second) {
            result.push_back(id);
        }
    }
    return result;
}

void inspect_source_reference(const SiteParcelState& state,
    const SiteParcelSourceReference& reference, SiteParcelRecordKind record_kind,
    std::uint64_t record_id, std::vector<SiteParcelDiagnostic>& diagnostics) {
    const auto* source = find_source(state, reference.kind, reference.identity);
    if (source == nullptr) {
        diagnostics.push_back({SiteParcelDiagnosticCode::missing_source_revision,
            record_kind, record_id, reference.identity, reference.revision, 0U});
    } else if (source->revision != reference.revision) {
        diagnostics.push_back({SiteParcelDiagnosticCode::stale_source_revision,
            record_kind, record_id, reference.identity, reference.revision, source->revision});
    }
}

} // namespace

SiteParcelModel::SiteParcelModel(SiteParcelModelId id, SiteParcelState state) noexcept
    : id_(id), state_(std::move(state)) {}

core::Result<SiteParcelModel> SiteParcelModel::create(
    SiteParcelModelId id, SiteParcelState state) {
    if (id == 0U) {
        return core::Result<SiteParcelModel>::failure(invalid(
            "site/parcel model identity must be non-zero"));
    }
    canonicalize(state);
    auto valid = validate_state(state);
    if (!valid) return core::Result<SiteParcelModel>::failure(valid.error());
    return core::Result<SiteParcelModel>::success(
        SiteParcelModel(id, std::move(state)));
}

core::Result<SiteParcelEditReceipt> SiteParcelModel::apply(
    std::uint64_t expected_revision, SiteParcelState replacement) {
    if (expected_revision != revision_) {
        return core::Result<SiteParcelEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data, "site/parcel edit revision is stale"));
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<SiteParcelEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "site/parcel model revision is exhausted"));
    }
    canonicalize(replacement);
    auto valid = validate_state(replacement);
    if (!valid) return core::Result<SiteParcelEditReceipt>::failure(valid.error());

    SiteParcelEditReceipt receipt;
    receipt.model_id = id_;
    receipt.revision_before = revision_;
    receipt.revision_after = revision_ + 1U;
    for (const auto& source : state_.current_sources) {
        const auto found = find_source(replacement, source.kind, source.identity);
        if (found == nullptr || *found != source) {
            receipt.changed_source_identities.push_back(source.identity);
        }
    }
    for (const auto& source : replacement.current_sources) {
        if (find_source(state_, source.kind, source.identity) == nullptr) {
            receipt.changed_source_identities.push_back(source.identity);
        }
    }
    std::sort(receipt.changed_source_identities.begin(),
        receipt.changed_source_identities.end());
    receipt.changed_source_identities.erase(std::unique(
        receipt.changed_source_identities.begin(),
        receipt.changed_source_identities.end()), receipt.changed_source_identities.end());
    receipt.changed_parcel_ids = changed_ids(state_.parcels, replacement.parcels,
        [](const SiteParcelRecord& value) { return value.id; });
    receipt.changed_frontage_ids = changed_ids(state_.frontages, replacement.frontages,
        [](const SiteParcelFrontage& value) { return value.id; });
    receipt.changed_access_ids = changed_ids(state_.accesses, replacement.accesses,
        [](const SiteParcelAccess& value) { return value.id; });
    receipt.changed_grade_relation_ids = changed_ids(state_.grade_relations,
        replacement.grade_relations,
        [](const SiteParcelGradeRelation& value) { return value.id; });
    receipt.changed_drainage_relation_ids = changed_ids(state_.drainage_relations,
        replacement.drainage_relations,
        [](const SiteParcelDrainageRelation& value) { return value.id; });

    state_ = std::move(replacement);
    ++revision_;
    return core::Result<SiteParcelEditReceipt>::success(std::move(receipt));
}

core::Result<SiteParcelReport> SiteParcelModel::inspect() const {
    SiteParcelReport report;
    report.model_id = id_;
    report.revision = revision_;

    std::unordered_map<SiteParcelId, std::size_t> frontage_counts;
    std::unordered_map<SiteParcelId, std::size_t> access_counts;
    frontage_counts.reserve(state_.frontages.size());
    access_counts.reserve(state_.accesses.size());
    for (const auto& frontage : state_.frontages) ++frontage_counts[frontage.parcel_id];
    for (const auto& access : state_.accesses) ++access_counts[access.parcel_id];

    for (const auto& parcel : state_.parcels) {
        if (!parcel.boundary) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::missing_boundary,
                SiteParcelRecordKind::parcel, parcel.id, {}, 0U, 0U});
        } else {
            inspect_source_reference(state_, *parcel.boundary,
                SiteParcelRecordKind::parcel, parcel.id, report.diagnostics);
        }
        if (!frontage_counts.contains(parcel.id)) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::missing_frontage,
                SiteParcelRecordKind::parcel, parcel.id, {}, 0U, 0U});
        }
        if (!access_counts.contains(parcel.id)) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::missing_access,
                SiteParcelRecordKind::parcel, parcel.id, {}, 0U, 0U});
        }
    }

    for (const auto& frontage : state_.frontages) {
        const auto parcel = std::lower_bound(state_.parcels.begin(), state_.parcels.end(),
            frontage.parcel_id,
            [](const SiteParcelRecord& record, SiteParcelId id) { return record.id < id; });
        if (frontage.boundary) {
            inspect_source_reference(state_, *frontage.boundary,
                SiteParcelRecordKind::frontage, frontage.id, report.diagnostics);
            if (parcel == state_.parcels.end() || parcel->id != frontage.parcel_id ||
                !parcel->boundary || *parcel->boundary != *frontage.boundary) {
                report.diagnostics.push_back({
                    SiteParcelDiagnosticCode::frontage_boundary_mismatch,
                    SiteParcelRecordKind::frontage, frontage.id,
                    frontage.boundary->identity, frontage.boundary->revision, 0U});
            }
        }
        if (!frontage.boundary || !frontage.road ||
            frontage.claim == SiteParcelFrontageClaim::unresolved) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::unresolved_frontage,
                SiteParcelRecordKind::frontage, frontage.id,
                frontage.road ? frontage.road->identity : std::string{},
                frontage.road ? frontage.road->revision : 0U, 0U});
        }
        if (frontage.road) {
            inspect_source_reference(state_, *frontage.road,
                SiteParcelRecordKind::frontage, frontage.id, report.diagnostics);
        }
    }

    for (const auto& access : state_.accesses) {
        if (!access.frontage_id || access.claim == SiteParcelAccessClaim::unresolved) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::unresolved_access,
                SiteParcelRecordKind::access, access.id, {}, 0U, 0U});
        } else {
            const auto frontage = std::lower_bound(state_.frontages.begin(),
                state_.frontages.end(), *access.frontage_id,
                [](const SiteParcelFrontage& record, SiteParcelFrontageId id) {
                    return record.id < id;
                });
            if (frontage == state_.frontages.end() || frontage->id != *access.frontage_id ||
                frontage->parcel_id != access.parcel_id) {
                report.diagnostics.push_back({
                    SiteParcelDiagnosticCode::missing_frontage_reference,
                    SiteParcelRecordKind::access, access.id, {}, 0U, 0U});
            }
        }
    }

    for (const auto& relation : state_.grade_relations) {
        if (relation.claim == SiteParcelGradeClaim::unresolved) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::unresolved_grade,
                SiteParcelRecordKind::grade_relation, relation.id, {}, 0U, 0U});
        }
        if (!relation.first_surface || !relation.second_surface) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::missing_grade_source,
                SiteParcelRecordKind::grade_relation, relation.id, {}, 0U, 0U});
        }
        if (relation.first_surface) {
            inspect_source_reference(state_, *relation.first_surface,
                SiteParcelRecordKind::grade_relation, relation.id, report.diagnostics);
        }
        if (relation.second_surface) {
            inspect_source_reference(state_, *relation.second_surface,
                SiteParcelRecordKind::grade_relation, relation.id, report.diagnostics);
        }
    }

    for (const auto& relation : state_.drainage_relations) {
        if (!relation.receiver || relation.claim == SiteParcelDrainageClaim::unresolved) {
            report.diagnostics.push_back({SiteParcelDiagnosticCode::unresolved_drainage,
                SiteParcelRecordKind::drainage_relation, relation.id, {}, 0U, 0U});
        }
        if (!relation.receiver) {
            report.diagnostics.push_back({
                SiteParcelDiagnosticCode::missing_drainage_receiver,
                SiteParcelRecordKind::drainage_relation, relation.id, {}, 0U, 0U});
        } else {
            inspect_source_reference(state_, *relation.receiver,
                SiteParcelRecordKind::drainage_relation, relation.id, report.diagnostics);
        }
    }

    std::sort(report.diagnostics.begin(), report.diagnostics.end(),
        [](const SiteParcelDiagnostic& left, const SiteParcelDiagnostic& right) {
            return std::tie(left.code, left.record_kind, left.record_id,
                       left.source_identity, left.expected_source_revision,
                       left.current_source_revision) <
                std::tie(right.code, right.record_kind, right.record_id,
                       right.source_identity, right.expected_source_revision,
                       right.current_source_revision);
        });
    return core::Result<SiteParcelReport>::success(std::move(report));
}

} // namespace carto::condition
