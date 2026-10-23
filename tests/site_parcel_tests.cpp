#include <carto/condition/site_parcel.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(expression);
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

using namespace carto::condition;

SiteParcelSourceReference reference(
    SiteParcelSourceKind kind, std::string identity, std::uint64_t revision) {
    return SiteParcelSourceReference{kind, std::move(identity), revision};
}

SiteParcelState complete_state() {
    SiteParcelState state;
    state.current_sources = {
        {SiteParcelSourceKind::parcel_boundary, "boundary-a", 4U},
        {SiteParcelSourceKind::parcel_boundary, "boundary-b", 2U},
        {SiteParcelSourceKind::road, "road-main", 7U},
        {SiteParcelSourceKind::grade_surface, "grade-a", 3U},
        {SiteParcelSourceKind::grade_surface, "grade-b", 5U},
        {SiteParcelSourceKind::drainage_receiver, "drainage-outfall", 1U},
    };
    state.parcels = {
        {11U, reference(SiteParcelSourceKind::parcel_boundary, "boundary-a", 4U)},
        {12U, reference(SiteParcelSourceKind::parcel_boundary, "boundary-b", 2U)},
    };
    state.frontages = {
        {21U, 11U,
            reference(SiteParcelSourceKind::parcel_boundary, "boundary-a", 4U),
            reference(SiteParcelSourceKind::road, "road-main", 7U),
            SiteParcelFrontageClaim::authored},
        {22U, 12U,
            reference(SiteParcelSourceKind::parcel_boundary, "boundary-b", 2U),
            reference(SiteParcelSourceKind::road, "road-main", 7U),
            SiteParcelFrontageClaim::authored},
    };
    state.accesses = {
        {31U, 11U, 21U, SiteParcelAccessKind::vehicle,
            SiteParcelAccessClaim::author_claims_available},
        {32U, 12U, 22U, SiteParcelAccessKind::pedestrian,
            SiteParcelAccessClaim::proposed},
    };
    state.grade_relations = {
        {41U, 11U, 12U,
            reference(SiteParcelSourceKind::grade_surface, "grade-a", 3U),
            reference(SiteParcelSourceKind::grade_surface, "grade-b", 5U),
            SiteParcelGradeClaim::author_claims_first_higher},
    };
    state.drainage_relations = {
        {51U, 11U,
            reference(SiteParcelSourceKind::drainage_receiver, "drainage-outfall", 1U),
            SiteParcelDrainageClaim::intended_to_receiver},
    };
    return state;
}

bool has_diagnostic(const SiteParcelReport& report,
    SiteParcelDiagnosticCode code, SiteParcelRecordKind record_kind,
    std::uint64_t record_id) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.code == code && diagnostic.record_kind == record_kind &&
            diagnostic.record_id == record_id) return true;
    }
    return false;
}

void authored_relationships_are_explicit_and_reviewable() {
    auto created = SiteParcelModel::create(5U, complete_state());
    REQUIRE(created);
    auto model = std::move(created.value());
    REQUIRE(model.id() == 5U);
    REQUIRE(model.revision() == 1U);
    REQUIRE(model.state().parcels.size() == 2U);
    REQUIRE(model.state().parcels[0].id == 11U);
    REQUIRE(model.state().frontages[0].road->identity == "road-main");
    REQUIRE(model.state().accesses[0].claim ==
        SiteParcelAccessClaim::author_claims_available);
    REQUIRE(model.state().grade_relations[0].claim ==
        SiteParcelGradeClaim::author_claims_first_higher);
    REQUIRE(model.state().drainage_relations[0].claim ==
        SiteParcelDrainageClaim::intended_to_receiver);

    const auto report = model.inspect();
    REQUIRE(report);
    REQUIRE(report.value().revision == 1U);
    REQUIRE(!report.value().needs_review());
    REQUIRE(report.value().diagnostics.empty());
}

void missing_and_stale_sources_are_reported_without_fallback() {
    auto state = complete_state();
    state.current_sources[2].revision = 8U;
    state.parcels[1].boundary.reset();
    state.frontages.pop_back();
    state.accesses[0].frontage_id = 999U;
    state.accesses.pop_back();
    state.accesses.push_back({33U, 11U, 21U, SiteParcelAccessKind::service,
        SiteParcelAccessClaim::unresolved});
    state.grade_relations[0].claim = SiteParcelGradeClaim::unresolved;
    state.grade_relations[0].second_surface.reset();
    state.drainage_relations[0].receiver.reset();
    state.frontages.push_back({23U, 11U,
        reference(SiteParcelSourceKind::parcel_boundary, "boundary-a", 4U),
        reference(SiteParcelSourceKind::road, "missing-road", 3U),
        SiteParcelFrontageClaim::authored});

    auto created = SiteParcelModel::create(6U, std::move(state));
    REQUIRE(created);
    const auto report = created.value().inspect();
    REQUIRE(report);
    REQUIRE(report.value().needs_review());
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::stale_source_revision,
        SiteParcelRecordKind::frontage, 21U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::missing_source_revision,
        SiteParcelRecordKind::frontage, 23U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::missing_boundary,
        SiteParcelRecordKind::parcel, 12U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::missing_frontage,
        SiteParcelRecordKind::parcel, 12U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::missing_access,
        SiteParcelRecordKind::parcel, 12U));
    REQUIRE(has_diagnostic(report.value(),
        SiteParcelDiagnosticCode::missing_frontage_reference,
        SiteParcelRecordKind::access, 31U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::unresolved_access,
        SiteParcelRecordKind::access, 33U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::unresolved_grade,
        SiteParcelRecordKind::grade_relation, 41U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::missing_grade_source,
        SiteParcelRecordKind::grade_relation, 41U));
    REQUIRE(has_diagnostic(report.value(), SiteParcelDiagnosticCode::unresolved_drainage,
        SiteParcelRecordKind::drainage_relation, 51U));
    REQUIRE(has_diagnostic(report.value(),
        SiteParcelDiagnosticCode::missing_drainage_receiver,
        SiteParcelRecordKind::drainage_relation, 51U));
    REQUIRE(created.value().state().frontages[0].road->revision == 7U);
}

void edits_are_revision_checked_atomic_and_keep_stable_ids() {
    auto created = SiteParcelModel::create(7U, complete_state());
    REQUIRE(created);
    auto model = std::move(created.value());

    auto replacement = model.state();
    replacement.current_sources[2].revision = 8U;
    for (auto& frontage : replacement.frontages) frontage.road->revision = 8U;
    const auto applied = model.apply(1U, replacement);
    REQUIRE(applied);
    REQUIRE(applied.value().revision_before == 1U);
    REQUIRE(applied.value().revision_after == 2U);
    REQUIRE(applied.value().changed_source_identities ==
        std::vector<std::string>{"road-main"});
    REQUIRE(applied.value().changed_frontage_ids ==
        (std::vector<std::uint64_t>{21U, 22U}));
    REQUIRE(model.state().parcels[0].id == 11U);
    REQUIRE(model.state().frontages[0].id == 21U);
    REQUIRE(!model.inspect().value().needs_review());

    const auto stale = model.apply(1U, complete_state());
    REQUIRE(!stale);
    REQUIRE(model.revision() == 2U);
    REQUIRE(model.state().current_sources[2].revision == 8U);

    auto dangling = model.state();
    dangling.parcels.erase(dangling.parcels.begin());
    const auto invalid = model.apply(2U, std::move(dangling));
    REQUIRE(!invalid);
    REQUIRE(model.revision() == 2U);
    REQUIRE(model.state().parcels.size() == 2U);
    REQUIRE(model.state().frontages[0].parcel_id == 11U);
}

void malformed_and_ambiguous_sources_fail_closed() {
    REQUIRE(!SiteParcelModel::create(0U));

    auto blank_source = complete_state();
    blank_source.current_sources[0].identity = " \t ";
    REQUIRE(!SiteParcelModel::create(8U, std::move(blank_source)));

    auto duplicate_source = complete_state();
    duplicate_source.current_sources.push_back(duplicate_source.current_sources.front());
    REQUIRE(!SiteParcelModel::create(8U, std::move(duplicate_source)));

    auto wrong_kind = complete_state();
    wrong_kind.parcels[0].boundary->kind = SiteParcelSourceKind::road;
    REQUIRE(!SiteParcelModel::create(8U, std::move(wrong_kind)));

    auto invalid_enum = complete_state();
    invalid_enum.accesses[0].claim = static_cast<SiteParcelAccessClaim>(255U);
    REQUIRE(!SiteParcelModel::create(8U, std::move(invalid_enum)));

    auto missing_endpoint = complete_state();
    missing_endpoint.grade_relations[0].second_parcel_id = 999U;
    REQUIRE(!SiteParcelModel::create(8U, std::move(missing_endpoint)));
}

} // namespace

int main() {
    try {
        authored_relationships_are_explicit_and_reviewable();
        missing_and_stale_sources_are_reported_without_fallback();
        edits_are_revision_checked_atomic_and_keep_stable_ids();
        malformed_and_ambiguous_sources_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
