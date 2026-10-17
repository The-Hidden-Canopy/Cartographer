#include <carto/engineering/authoring.hpp>
#include <carto/engineering/cad_provider.hpp>
#include <carto/application/application.hpp>
#include <carto/ui/ui.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void fixture_round_trips_with_identity_and_provenance() {
    auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    REQUIRE(fixture.value().sources.size() == 2U);
    REQUIRE(fixture.value().occurrences.size() == 3U);
    REQUIRE(fixture.value().topology.size() == 3U);
    REQUIRE(fixture.value().requirements.size() == 3U);
    REQUIRE(fixture.value().characteristics.size() == 3U);
    REQUIRE(fixture.value().targets.size() == 4U);
    REQUIRE(fixture.value().targets.at(1U).status == carto::engineering::TargetStatus::rejected);
    REQUIRE(fixture.value().requirements.at(1U).extraction ==
            carto::engineering::ExtractionState::human_corrected);

    const auto bytes = fixture.value().serialize();
    const auto digest = fixture.value().canonical_digest();
    const auto reopened = carto::engineering::AuthoringDocument::deserialize(bytes);
    if (!reopened) throw TestFailure(reopened.error().message);
    REQUIRE(digest);
    REQUIRE(reopened.value().serialize() == bytes);
    REQUIRE(reopened.value().canonical_digest().value() == digest.value());
    REQUIRE(reopened.value().topology.at(0U).display_mesh_asset == 1000U);
}

void bom_csv_parser_and_reconciliation_fail_closed_on_ambiguous_evidence() {
    const auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    const std::string valid_csv =
        "part_number,revision,description,material,quantity,unit\r\n"
        "BRACKET-A17,C,\"Machined, bracket\",7075-T6,2,each\r\n"
        "PIN-P04,B,\"Pin \"\"P04\"\"\",17-4PH,1,each\r\n";
    const auto parsed = carto::engineering::parse_bom_csv(valid_csv, 3U, 900U);
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 2U);
    REQUIRE(parsed.value().front().description == "Machined, bracket");
    REQUIRE(parsed.value().back().description == "Pin \"P04\"");
    REQUIRE(parsed.value().front().id == 900U);
    const auto reconciled = carto::engineering::reconcile_bom_lines(
        fixture.value(), parsed.value(), 3U, 902U);
    REQUIRE(reconciled);
    REQUIRE(reconciled.value().size() == 2U);
    REQUIRE(reconciled.value().at(0U).reconciliation ==
        carto::engineering::ReconciliationStatus::exact);
    REQUIRE(reconciled.value().at(1U).reconciliation ==
        carto::engineering::ReconciliationStatus::exact);
    REQUIRE(reconciled.value().at(0U).mapped_part_id == 10U);

    const auto incomplete = carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,2,each\n", 4U, 1000U);
    REQUIRE(incomplete);
    const auto with_assembly_only = carto::engineering::reconcile_bom_lines(
        fixture.value(), incomplete.value(), 4U, 1001U);
    REQUIRE(with_assembly_only);
    REQUIRE(with_assembly_only.value().size() == 2U);
    REQUIRE(with_assembly_only.value().at(1U).part_number == "PIN-P04");
    REQUIRE(with_assembly_only.value().at(1U).reconciliation ==
        carto::engineering::ReconciliationStatus::assembly_only);

    const auto unknown_part = carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "NOT-IN-ASSEMBLY,A,Unknown,,1,each\n", 5U, 1100U);
    REQUIRE(unknown_part);
    const auto source_only = carto::engineering::reconcile_bom_lines(
        fixture.value(), unknown_part.value(), 5U, 1101U);
    REQUIRE(source_only);
    REQUIRE(source_only.value().front().reconciliation ==
        carto::engineering::ReconciliationStatus::source_only);

    auto duplicate_part = fixture.value();
    duplicate_part.parts.push_back({12U, "BRACKET-A17", "Second bracket record", "C",
        "7075-T6", std::nullopt, 1U});
    REQUIRE(duplicate_part.validate());
    const auto ambiguous = carto::engineering::reconcile_bom_lines(
        duplicate_part, incomplete.value(), 4U, 1001U);
    REQUIRE(ambiguous);
    REQUIRE(ambiguous.value().front().reconciliation ==
        carto::engineering::ReconciliationStatus::ambiguous);

    const auto revision_mismatch = carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,D,Bracket,7075-T6,2,each\n", 6U, 1200U);
    REQUIRE(revision_mismatch);
    REQUIRE(carto::engineering::reconcile_bom_lines(
        fixture.value(), revision_mismatch.value(), 6U, 1201U).value().front().reconciliation ==
        carto::engineering::ReconciliationStatus::revision_mismatch);
    const auto material_mismatch = carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,6061-T6,2,each\n", 7U, 1300U);
    REQUIRE(material_mismatch);
    REQUIRE(carto::engineering::reconcile_bom_lines(
        fixture.value(), material_mismatch.value(), 7U, 1301U).value().front().reconciliation ==
        carto::engineering::ReconciliationStatus::material_mismatch);
    const auto quantity_mismatch = carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,3,each\n", 8U, 1400U);
    REQUIRE(quantity_mismatch);
    REQUIRE(carto::engineering::reconcile_bom_lines(
        fixture.value(), quantity_mismatch.value(), 8U, 1401U).value().front().reconciliation ==
        carto::engineering::ReconciliationStatus::quantity_mismatch);
    const auto unsupported_unit = carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,2,kg\n", 9U, 1500U);
    REQUIRE(unsupported_unit);
    REQUIRE(carto::engineering::reconcile_bom_lines(
        fixture.value(), unsupported_unit.value(), 9U, 1501U).value().front().reconciliation ==
        carto::engineering::ReconciliationStatus::unresolved);

    REQUIRE(!carto::engineering::parse_bom_csv(
        "part_number,description,revision,material,quantity,unit\n"
        "BRACKET-A17,Bracket,C,7075-T6,2,each\n", 3U, 1U));
    REQUIRE(!carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,\"unterminated,7075-T6,2,each\n", 3U, 1U));
    REQUIRE(!carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,NaN,each\n", 3U, 1U));
    REQUIRE(!carto::engineering::parse_bom_csv(
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,2,each,extra\n", 3U, 1U));
    std::string invalid_utf8 =
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,2,each\n";
    invalid_utf8[invalid_utf8.find("Bracket")] = static_cast<char>(0xff);
    REQUIRE(!carto::engineering::parse_bom_csv(invalid_utf8, 3U, 1U));
    REQUIRE(!carto::engineering::parse_bom_csv(
        std::string(carto::engineering::kMaxBomCsvBytes + 1U, 'x'), 3U, 1U));

    REQUIRE(carto::engineering::inspection_kind_compatible(
        carto::engineering::RequirementKind::diameter,
        carto::engineering::InspectionKind::diameter));
    REQUIRE(!carto::engineering::inspection_kind_compatible(
        carto::engineering::RequirementKind::diameter,
        carto::engineering::InspectionKind::visual_surface_check));
    REQUIRE(!carto::engineering::inspection_kind_compatible(
        carto::engineering::RequirementKind::material_verification,
        carto::engineering::InspectionKind::feature_presence));

    auto cyclic_occurrences = fixture.value();
    cyclic_occurrences.occurrences.at(0U).parent_id = 21U;
    cyclic_occurrences.occurrences.at(1U).parent_id = 20U;
    REQUIRE(!carto::engineering::assembly_quantity_for_part(cyclic_occurrences, 10U));
}

void publication_and_vanta_admission_bind_exact_bytes() {
    const auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    auto package = carto::engineering::InspectionPackage::publish(
        fixture.value(), fixture.value().project_revision, fixture.value().project_namespace);
    REQUIRE(package);
    REQUIRE(package.value().validate());
    const auto serialized = package.value().serialize();
    const auto reopened = carto::engineering::InspectionPackage::deserialize(serialized);
    if (!reopened) throw TestFailure(reopened.error().message);
    REQUIRE(reopened.value().publication_digest == package.value().publication_digest);
    const auto admission = carto::engineering::admit_for_vanta(reopened.value());
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::engineering::VantaAdmissionStatus::admit);
    REQUIRE(admission.value().admitted_authoring_digest == package.value().authoring_digest);
    REQUIRE(admission.value().admitted_publication_digest == package.value().publication_digest);

    auto tampered = reopened.value();
    tampered.authoring_bytes.push_back('x');
    REQUIRE(!tampered.validate());

    auto malformed_binding = fixture.value();
    malformed_binding.characteristics.front().topology_ids = {102U};
    REQUIRE(!malformed_binding.validate());

    auto missing_characteristic = fixture.value();
    missing_characteristic.requirements.push_back({
        303U, carto::engineering::RequirementKind::dimensional_distance,
        carto::engineering::ExtractionState::manual,
        {1U, 34U, "Manual", "DIM-NEW", "B5", "", 0.0, "9.00 +/- 0.10 mm"},
        {100U}, 9.0, 0.1, 0.1, "millimeters", {}, {},
        "within bilateral tolerance", true});
    REQUIRE(missing_characteristic.validate());
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        missing_characteristic, missing_characteristic.project_revision,
        missing_characteristic.project_namespace));
}

void manual_requirement_authoring_is_revision_bound_and_auditable() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Manual Requirement"}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportEngineeringFixtureAction{}));
    carto::ui::UiController ui(session);

    const auto before = session.snapshot();
    carto::application::CreateEngineeringRequirementAction action;
    action.expected_project_revision = before.project_revision;
    action.expected_authoring_revision = before.engineering.authored_revision;
    action.source_id = 2U;
    action.topology_ids = {100U};
    action.kind = carto::engineering::RequirementKind::dimensional_distance;
    action.nominal = 9.0;
    action.plus = 0.1;
    action.minus = 0.1;
    action.unit = "millimeters";
    action.source_page = 34U;
    action.source_section = "Manual correction";
    action.source_callout = "DIM-NEW";
    action.original_text = "9.00 +/- 0.10 mm";
    action.acceptance_rule = "within bilateral tolerance";
    const auto created = ui.dispatch(action);
    REQUIRE(created);
    REQUIRE(created.value().document_changed);
    REQUIRE(session.snapshot().project_revision == before.project_revision.next());
    REQUIRE(session.snapshot().engineering.requirement_count == 4U);
    REQUIRE(session.snapshot().engineering.requirements.back().extraction == "manual");
    REQUIRE(session.snapshot().engineering.requirements.back().source_id == 2U);
    REQUIRE(session.snapshot().engineering.requirements.back().topology_ids ==
        std::vector<std::uint64_t>{100U});
    REQUIRE(session.snapshot().engineering.requirements.back().original_text ==
        "9.00 +/- 0.10 mm");
    REQUIRE(!ui.snapshot().operations.empty());
    REQUIRE(ui.snapshot().operations.back().action ==
        "Create Manual Engineering Requirement");
    REQUIRE(!session.snapshot().engineering.publication_ready);

    const auto after_create = session.snapshot();
    auto stale_action = action;
    stale_action.expected_project_revision = before.project_revision;
    const auto stale = carto::application::HumanApplicationAccess::dispatch(
        session, stale_action);
    REQUIRE(!stale);
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);

    auto missing_source = action;
    missing_source.expected_project_revision = after_create.project_revision;
    missing_source.expected_authoring_revision = after_create.engineering.authored_revision;
    missing_source.source_id = 999U;
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(session, missing_source));

    auto missing_original_text = action;
    missing_original_text.expected_project_revision = after_create.project_revision;
    missing_original_text.expected_authoring_revision = after_create.engineering.authored_revision;
    missing_original_text.original_text.clear();
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, missing_original_text));

    auto missing_acceptance_rule = action;
    missing_acceptance_rule.expected_project_revision = after_create.project_revision;
    missing_acceptance_rule.expected_authoring_revision = after_create.engineering.authored_revision;
    missing_acceptance_rule.acceptance_rule.clear();
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, missing_acceptance_rule));

    auto blank_acceptance_rule = missing_acceptance_rule;
    blank_acceptance_rule.acceptance_rule = "   ";
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, blank_acceptance_rule));

    auto non_finite_nominal = action;
    non_finite_nominal.expected_project_revision = after_create.project_revision;
    non_finite_nominal.expected_authoring_revision = after_create.engineering.authored_revision;
    non_finite_nominal.nominal = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, non_finite_nominal));

    auto negative_tolerance = action;
    negative_tolerance.expected_project_revision = after_create.project_revision;
    negative_tolerance.expected_authoring_revision = after_create.engineering.authored_revision;
    negative_tolerance.plus = -0.01;
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, negative_tolerance));

    auto duplicate_topology = action;
    duplicate_topology.expected_project_revision = after_create.project_revision;
    duplicate_topology.expected_authoring_revision = after_create.engineering.authored_revision;
    duplicate_topology.topology_ids = {100U, 100U};
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, duplicate_topology));

    auto unsupported_kind = action;
    unsupported_kind.expected_project_revision = after_create.project_revision;
    unsupported_kind.expected_authoring_revision = after_create.engineering.authored_revision;
    unsupported_kind.kind = static_cast<carto::engineering::RequirementKind>(255);
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(
        session, unsupported_kind));
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);

    auto missing_topology = action;
    missing_topology.expected_project_revision = after_create.project_revision;
    missing_topology.expected_authoring_revision = after_create.engineering.authored_revision;
    missing_topology.topology_ids = {999U};
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(session, missing_topology));
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);

    auto invalid_unit = action;
    invalid_unit.expected_project_revision = after_create.project_revision;
    invalid_unit.expected_authoring_revision = after_create.engineering.authored_revision;
    invalid_unit.unit = "degrees";
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(session, invalid_unit));
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);

    REQUIRE(!ui.submit_ai_proposal(stale_action));
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().engineering.requirement_count == 3U);
    REQUIRE(session.snapshot().engineering.publication_ready);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().engineering.requirement_count == 4U);
    REQUIRE(!session.snapshot().engineering.publication_ready);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::InvalidateEngineeringSourceAction{
            1U, "DV012-rev-d", digest("dv012-rev-d")}));
    auto stale_binding = action;
    stale_binding.expected_project_revision = session.snapshot().project_revision;
    stale_binding.expected_authoring_revision = session.snapshot().engineering.authored_revision;
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(session, stale_binding));
    REQUIRE(session.snapshot().engineering.stale);
}

void inspection_characteristics_require_current_human_verified_contracts() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Characteristic authoring"}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportEngineeringFixtureAction{}));
    carto::ui::UiController ui(session);

    auto before_requirement = session.snapshot();
    carto::application::CreateEngineeringRequirementAction requirement;
    requirement.expected_project_revision = before_requirement.project_revision;
    requirement.expected_authoring_revision = before_requirement.engineering.authored_revision;
    requirement.source_id = 1U;
    requirement.topology_ids = {100U};
    requirement.kind = carto::engineering::RequirementKind::dimensional_distance;
    requirement.nominal = 6.0;
    requirement.plus = 0.1;
    requirement.minus = 0.1;
    requirement.unit = "millimeters";
    requirement.source_section = "Manual inspection plan";
    requirement.source_callout = "DIST-NEW";
    requirement.original_text = "6.00 +/- 0.10 mm";
    requirement.acceptance_rule = "within stated limits";
    REQUIRE(ui.dispatch(requirement));
    const auto after_requirement = session.snapshot();
    REQUIRE(after_requirement.engineering.requirement_count == 4U);

    const auto make_action = [](const carto::application::ApplicationSnapshot& snapshot) {
        carto::application::CreateEngineeringCharacteristicAction action;
        action.expected_project_revision = snapshot.project_revision;
        action.expected_authoring_revision = snapshot.engineering.authored_revision;
        action.requirement_id = 303U;
        action.kind = carto::engineering::InspectionKind::distance;
        action.sensor = {"structured-light", "tool0", 0.10, 0.35, 0.20,
            25.0, 60.0, 0.05, true, false, 0.5};
        action.coverage = {"single_point", 0.0};
        action.access = {{"+z"}, {"-x"}, 0.05, false, false};
        action.priority = 12U;
        return action;
    };
    auto valid_action = make_action(after_requirement);
    REQUIRE(!ui.submit_ai_proposal(valid_action));
    REQUIRE(session.snapshot().project_revision == after_requirement.project_revision);

    auto wrong_kind = valid_action;
    wrong_kind.kind = carto::engineering::InspectionKind::diameter;
    REQUIRE(!ui.dispatch(wrong_kind));
    auto missing_sensor = valid_action;
    missing_sensor.sensor.sensor_type.clear();
    REQUIRE(!ui.dispatch(missing_sensor));
    auto missing_approach = valid_action;
    missing_approach.access.allowed_approach_directions.clear();
    REQUIRE(!ui.dispatch(missing_approach));
    auto contradictory_approach = valid_action;
    contradictory_approach.access.forbidden_approach_directions = {"+z"};
    REQUIRE(!ui.dispatch(contradictory_approach));
    auto unsupported_approach = valid_action;
    unsupported_approach.access.allowed_approach_directions = {"toward-datum"};
    REQUIRE(!ui.dispatch(unsupported_approach));
    auto zero_range_sensor = valid_action;
    zero_range_sensor.sensor.max_standoff = 0.0;
    REQUIRE(!ui.dispatch(zero_range_sensor));
    REQUIRE(session.snapshot().project_revision == after_requirement.project_revision);

    const auto created = ui.create_engineering_characteristic(valid_action);
    REQUIRE(created && created.value().document_changed);
    auto after_create = session.snapshot();
    REQUIRE(after_create.project_revision == after_requirement.project_revision.next());
    REQUIRE(after_create.engineering.characteristic_count == 4U);
    const auto characteristic = std::find_if(after_create.engineering.characteristics.begin(),
        after_create.engineering.characteristics.end(), [](const auto& value) {
            return value.requirement_id == 303U;
        });
    REQUIRE(characteristic != after_create.engineering.characteristics.end());
    REQUIRE(characteristic->kind == "distance");
    REQUIRE(characteristic->sensor_type == "structured-light");
    REQUIRE(characteristic->topology_ids == std::vector<std::uint64_t>{100U});
    const auto authored_document = session.engineering_authoring();
    REQUIRE(authored_document);
    REQUIRE(authored_document.value().path->status ==
        carto::engineering::DerivedProductStatus::stale);
    REQUIRE(std::all_of(authored_document.value().path->samples.begin(),
        authored_document.value().path->samples.end(), [](const auto& sample) {
            return sample.status == carto::engineering::PathSampleStatus::stale;
        }));
    REQUIRE(std::all_of(authored_document.value().derived_products.begin(),
        authored_document.value().derived_products.end(), [](const auto& product) {
            return product.status == carto::engineering::DerivedProductStatus::stale &&
                !product.stale_reason.empty();
        }));
    REQUIRE(!ui.snapshot().operations.empty());
    REQUIRE(ui.snapshot().operations.back().action == "Create Inspection Characteristic");
    REQUIRE(ui.snapshot().operations.back().category ==
        carto::ui::OperationCategory::committed_command);

    auto duplicate = make_action(after_create);
    REQUIRE(!ui.dispatch(duplicate));
    auto stale = valid_action;
    REQUIRE(!ui.dispatch(stale));
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().engineering.characteristic_count == 3U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().engineering.characteristic_count == 4U);

    REQUIRE(ui.dispatch(carto::application::InvalidateEngineeringSourceAction{
        1U, "DV012-rev-d", digest("dv012-rev-d-characteristic" )}));
    auto stale_source_action = make_action(session.snapshot());
    REQUIRE(!ui.dispatch(stale_source_action));
    REQUIRE(session.snapshot().engineering.stale);
}

void inspection_target_review_is_human_revision_bound_and_preserves_decisions() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Target Review"}));
    auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    fixture.value().project_revision = session.snapshot().project_revision.next();
    fixture.value().targets.at(0U).status = carto::engineering::TargetStatus::proposed;
    fixture.value().targets.at(1U).status = carto::engineering::TargetStatus::proposed;
    fixture.value().targets.at(3U).status = carto::engineering::TargetStatus::proposed;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportEngineeringAuthoringAction{
            fixture.value().serialize()}));
    carto::ui::UiController ui(session);

    const auto before = session.snapshot();
    carto::application::ReviewEngineeringTargetAction accept{
        before.project_revision, before.engineering.authored_revision, 500U,
        carto::engineering::TargetStatus::accepted};
    REQUIRE(!ui.dispatch(carto::application::ReviewEngineeringTargetAction{
        before.project_revision, before.engineering.authored_revision, 500U,
        carto::engineering::TargetStatus::runtime_verified}));
    REQUIRE(!ui.submit_ai_proposal(accept));
    REQUIRE(session.snapshot().project_revision == before.project_revision);

    const auto accepted = ui.dispatch(accept);
    REQUIRE(accepted && accepted.value().document_changed);
    REQUIRE(session.snapshot().project_revision == before.project_revision.next());
    REQUIRE(session.snapshot().engineering.authored_revision ==
        before.engineering.authored_revision.next());
    const auto accepted_snapshot = session.snapshot();
    const auto accepted_target = std::find_if(
        accepted_snapshot.engineering.targets.begin(),
        accepted_snapshot.engineering.targets.end(), [](const auto& target) {
            return target.id == 500U;
        });
    REQUIRE(accepted_target != accepted_snapshot.engineering.targets.end());
    REQUIRE(accepted_target->status == "accepted");
    REQUIRE(!ui.snapshot().operations.empty());
    REQUIRE(ui.snapshot().operations.back().action == "Accept Inspection Target");

    auto stale = accept;
    stale.decision = carto::engineering::TargetStatus::rejected;
    REQUIRE(!ui.dispatch(stale));
    REQUIRE(!ui.dispatch(carto::application::ReviewEngineeringTargetAction{
        session.snapshot().project_revision,
        session.snapshot().engineering.authored_revision,
        500U, carto::engineering::TargetStatus::rejected}));
    REQUIRE(session.snapshot().project_revision == before.project_revision.next());

    const auto current = session.snapshot();
    const auto rejected = ui.dispatch(carto::application::ReviewEngineeringTargetAction{
        current.project_revision, current.engineering.authored_revision, 501U,
        carto::engineering::TargetStatus::rejected});
    REQUIRE(rejected && rejected.value().document_changed);
    const auto rejected_snapshot = session.snapshot();
    const auto rejected_target = std::find_if(
        rejected_snapshot.engineering.targets.begin(),
        rejected_snapshot.engineering.targets.end(), [](const auto& target) {
            return target.id == 501U;
        });
    REQUIRE(rejected_target != rejected_snapshot.engineering.targets.end());
    REQUIRE(rejected_target->status == "rejected");
    REQUIRE(session.snapshot().engineering.target_count == 4U);
    REQUIRE(ui.snapshot().operations.back().action == "Reject Inspection Target");

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    const auto undone_snapshot = session.snapshot();
    const auto undone_target = std::find_if(
        undone_snapshot.engineering.targets.begin(),
        undone_snapshot.engineering.targets.end(), [](const auto& target) {
            return target.id == 501U;
        });
    REQUIRE(undone_target != undone_snapshot.engineering.targets.end());
    REQUIRE(undone_target->status == "proposed");
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    const auto redone_snapshot = session.snapshot();
    const auto redone_target = std::find_if(
        redone_snapshot.engineering.targets.begin(),
        redone_snapshot.engineering.targets.end(), [](const auto& target) {
            return target.id == 501U;
        });
    REQUIRE(redone_target != redone_snapshot.engineering.targets.end());
    REQUIRE(redone_target->status == "rejected");

    const auto before_invalidation = session.snapshot();
    REQUIRE(ui.dispatch(carto::application::InvalidateEngineeringSourceAction{
        1U, "DV012-rev-d", digest("dv012-rev-d")}));
    const auto stale_snapshot = session.snapshot();
    REQUIRE(stale_snapshot.project_revision == before_invalidation.project_revision.next());
    const auto stale_target = std::find_if(
        stale_snapshot.engineering.targets.begin(),
        stale_snapshot.engineering.targets.end(), [](const auto& target) {
            return target.id == 503U;
        });
    REQUIRE(stale_target != stale_snapshot.engineering.targets.end());
    REQUIRE(stale_target->status == "stale");
    REQUIRE(!ui.dispatch(carto::application::ReviewEngineeringTargetAction{
        stale_snapshot.project_revision, stale_snapshot.engineering.authored_revision,
        503U, carto::engineering::TargetStatus::accepted}));
    REQUIRE(session.snapshot().project_revision == stale_snapshot.project_revision);
}

void inspection_target_authoring_is_human_revision_and_workcell_bound() {
    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Target authoring"}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportEngineeringFixtureAction{}));
    carto::ui::UiController ui(session);

    const auto make_action = [](const carto::application::ApplicationSnapshot& snapshot) {
        carto::application::CreateEngineeringTargetAction action;
        action.expected_project_revision = snapshot.project_revision;
        action.expected_authoring_revision = snapshot.engineering.authored_revision;
        action.characteristic_id = 400U;
        action.source_topology_id = 101U;
        action.sensor_pose = {{0.12, 0.13, 0.14}, {0.0, 0.0, 0.0, 2.0},
            {1.0, 1.0, 1.0}};
        action.standoff = 0.20;
        action.pose_frame = "workcell";
        action.justification = "Manual inspection plan callout DIST-17";
        return action;
    };

    const auto before = session.snapshot();
    REQUIRE(before.engineering.workcell_present);
    const auto valid_action = make_action(before);
    REQUIRE(!ui.submit_ai_proposal(valid_action));

    auto missing_justification = valid_action;
    missing_justification.justification.clear();
    REQUIRE(!ui.dispatch(missing_justification));
    auto multiline_justification = valid_action;
    multiline_justification.justification = "line one\nline two";
    REQUIRE(!ui.dispatch(multiline_justification));
    auto wrong_frame = valid_action;
    wrong_frame.pose_frame = "robot-base";
    REQUIRE(!ui.dispatch(wrong_frame));
    auto wrong_topology = valid_action;
    wrong_topology.source_topology_id = 102U;
    REQUIRE(!ui.dispatch(wrong_topology));
    auto out_of_range = valid_action;
    out_of_range.standoff = 0.09;
    REQUIRE(!ui.dispatch(out_of_range));
    auto zero_noncontact_standoff = valid_action;
    zero_noncontact_standoff.standoff = 0.0;
    REQUIRE(!ui.dispatch(zero_noncontact_standoff));
    auto unnormalizable_pose = valid_action;
    unnormalizable_pose.sensor_pose.rotation = {0.0, 0.0, 0.0, 0.0};
    REQUIRE(!ui.dispatch(unnormalizable_pose));
    auto scaled_pose = valid_action;
    scaled_pose.sensor_pose.scale = {1.0, 2.0, 1.0};
    REQUIRE(!ui.dispatch(scaled_pose));
    auto stale_project = valid_action;
    stale_project.expected_project_revision = stale_project.expected_project_revision.next();
    REQUIRE(!ui.dispatch(stale_project));
    auto stale_authoring = valid_action;
    stale_authoring.expected_authoring_revision =
        stale_authoring.expected_authoring_revision.next();
    REQUIRE(!ui.dispatch(stale_authoring));
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().engineering.target_count == before.engineering.target_count);

    const auto created = ui.create_engineering_target(valid_action);
    REQUIRE(created && created.value().document_changed);
    auto after_create = session.snapshot();
    REQUIRE(after_create.project_revision == before.project_revision.next());
    REQUIRE(after_create.engineering.target_count == before.engineering.target_count + 1U);
    REQUIRE(!after_create.engineering.publication_ready);
    const auto authored = session.engineering_authoring();
    REQUIRE(authored);
    const auto target = std::find_if(authored.value().targets.begin(),
        authored.value().targets.end(), [](const auto& value) {
            return value.id == 504U;
        });
    REQUIRE(target != authored.value().targets.end());
    REQUIRE(target->status == carto::engineering::TargetStatus::proposed);
    REQUIRE(target->human_authored);
    REQUIRE(target->sensor_pose.translation.x == 0.12);
    REQUIRE(target->sensor_pose.rotation.w == 1.0);
    REQUIRE(target->generation_basis.find("workcell-revision=C") != std::string::npos);
    REQUIRE(target->generation_basis.find(valid_action.justification) != std::string::npos);
    REQUIRE(authored.value().path->status ==
        carto::engineering::DerivedProductStatus::stale);
    REQUIRE(!ui.snapshot().operations.empty());
    REQUIRE(ui.snapshot().operations.back().action == "Create Proposed Inspection Target");
    REQUIRE(ui.snapshot().operations.back().category ==
        carto::ui::OperationCategory::committed_command);

    auto duplicate = make_action(after_create);
    REQUIRE(!ui.dispatch(duplicate));
    REQUIRE(session.snapshot().project_revision == after_create.project_revision);

    auto changed_workcell = authored.value();
    changed_workcell.project_revision = changed_workcell.project_revision.next();
    changed_workcell.workcell->revision = "D";
    REQUIRE(ui.dispatch(carto::application::ImportEngineeringAuthoringAction{
        changed_workcell.serialize()}));
    const auto changed_snapshot = session.snapshot();
    const auto blocked_review = ui.dispatch(carto::application::ReviewEngineeringTargetAction{
        changed_snapshot.project_revision, changed_snapshot.engineering.authored_revision,
        504U, carto::engineering::TargetStatus::accepted});
    REQUIRE(!blocked_review);
    REQUIRE(session.snapshot().project_revision == changed_snapshot.project_revision);
    const auto still_proposed = session.engineering_authoring();
    REQUIRE(still_proposed);
    REQUIRE(still_proposed.value().targets.back().status ==
        carto::engineering::TargetStatus::proposed);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    after_create = session.snapshot();
    const auto accepted = ui.dispatch(carto::application::ReviewEngineeringTargetAction{
        after_create.project_revision, after_create.engineering.authored_revision,
        504U, carto::engineering::TargetStatus::accepted});
    REQUIRE(accepted && accepted.value().document_changed);
    REQUIRE(session.snapshot().engineering.authored_revision ==
        after_create.engineering.authored_revision.next());
    REQUIRE(ui.snapshot().operations.back().action == "Accept Inspection Target");

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    const auto undone = session.engineering_authoring();
    REQUIRE(undone);
    REQUIRE(undone.value().targets.back().status ==
        carto::engineering::TargetStatus::proposed);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    const auto redone = session.engineering_authoring();
    REQUIRE(redone);
    REQUIRE(redone.value().targets.back().status ==
        carto::engineering::TargetStatus::accepted);

    const auto before_invalidation = session.snapshot();
    REQUIRE(ui.dispatch(carto::application::InvalidateEngineeringSourceAction{
        1U, "DV012-rev-d", digest("dv012-rev-d-target-authoring")}));
    const auto stale_snapshot = session.snapshot();
    REQUIRE(stale_snapshot.project_revision == before_invalidation.project_revision.next());
    const auto stale_target = std::find_if(stale_snapshot.engineering.targets.begin(),
        stale_snapshot.engineering.targets.end(), [](const auto& value) {
            return value.id == 504U;
        });
    REQUIRE(stale_target != stale_snapshot.engineering.targets.end());
    REQUIRE(stale_target->status == "stale");
}

void application_wires_engineering_authoring_through_persistence_and_history() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto cleanup = [&]() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    };

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"DV012 Acceptance"}));
    const auto imported = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportEngineeringFixtureAction{});
    REQUIRE(imported);
    REQUIRE(imported.value().document_changed);
    REQUIRE(session.snapshot().engineering.present);
    REQUIRE(session.snapshot().engineering.publication_ready);
    REQUIRE(session.snapshot().engineering.vanta_contract_admissible);
    REQUIRE(session.snapshot().engineering.requirement_count == 3U);
    REQUIRE(session.snapshot().engineering.target_count == 4U);
    REQUIRE(session.snapshot().engineering.status.find("local VANTA handoff-contract") !=
        std::string::npos);

    const auto project_path = root / "dv012.carto";
    const auto saved = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{project_path});
    if (!saved) throw TestFailure(saved.error().message + " [" + project_path.string() + "]");

    std::ifstream project_stream(project_path, std::ios::binary);
    REQUIRE(project_stream.good());
    std::string project_bytes((std::istreambuf_iterator<char>(project_stream)), {});
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(carto::project::ProjectDocument::kSchemaVersion) + "\n";
    REQUIRE(project_bytes.starts_with(current_header));
    project_bytes.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 13\n");
    constexpr std::string_view empty_condition_record = "CONDITION_AUTHORING NONE\n";
    const std::size_t condition_record = project_bytes.find(empty_condition_record);
    REQUIRE(condition_record != std::string::npos);
    project_bytes.erase(condition_record, empty_condition_record.size());
    REQUIRE(!carto::project::ProjectDocument::deserialize(project_bytes));

    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened.snapshot().engineering.present);
    REQUIRE(reopened.snapshot().engineering.digest == session.snapshot().engineering.digest);
    REQUIRE(reopened.snapshot().engineering.publication_ready);

    const auto invalidated = carto::application::HumanApplicationAccess::dispatch(
        reopened,
        carto::application::InvalidateEngineeringSourceAction{
            1U, "DV012-rev-b", digest("dv012-rev-b")});
    REQUIRE(invalidated);
    REQUIRE(!reopened.snapshot().engineering.publication_ready);
    REQUIRE(reopened.snapshot().engineering.status.find("blocked") != std::string::npos);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::UndoAction{}));
    REQUIRE(reopened.snapshot().engineering.publication_ready);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::RedoAction{}));
    REQUIRE(!reopened.snapshot().engineering.publication_ready);

    cleanup();
}

void ui_exposes_engineering_workspace_and_immutable_publication() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-ui-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"DV012 UI"}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::engineering));
    REQUIRE(ui.snapshot().workspace_kind == carto::ui::Workspace::engineering);
    REQUIRE(ui.import_engineering_fixture());
    const auto snapshot = ui.snapshot();
    REQUIRE(snapshot.engineering.present);
    REQUIRE(snapshot.engineering.publication_ready);
    REQUIRE(snapshot.engineering.vanta_contract_admissible);
    REQUIRE(snapshot.engineering.topology.size() == 3U);
    REQUIRE(snapshot.engineering.topology.front().external_id == "FACE-A17-OUTER");
    REQUIRE(snapshot.engineering.requirements.size() == 3U);
    REQUIRE(snapshot.engineering.requirements.front().original_text ==
        "12.50 +/- 0.05 mm");

    const auto package = ui.build_engineering_package();
    REQUIRE(package);
    const auto admission = ui.admit_engineering_package();
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::engineering::VantaAdmissionStatus::admit);
    const auto path = root / "inspection.carto-inspection";
    REQUIRE(ui.publish_engineering_package(path));
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    const auto reopened = carto::engineering::InspectionPackage::deserialize(bytes);
    REQUIRE(reopened);
    REQUIRE(reopened.value().publication_digest == package.value().publication_digest);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_imports_bom_csv_through_revision_bound_journal_and_preserves_source_history() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-bom-io-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto csv_path = root / "assembly-bom.csv";
    const std::string initial_csv =
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,2,each\n"
        "PIN-P04,B,Pin,17-4PH,1,each\n";
    {
        std::ofstream stream(csv_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << initial_csv;
        REQUIRE(stream.good());
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"BOM CSV import"}));
    REQUIRE(ui.import_engineering_fixture());
    const auto before = ui.snapshot();
    const auto ai_action = carto::application::ImportEngineeringBomCsvAction{
        before.project_revision, before.engineering.authored_revision,
        csv_path.generic_string(), initial_csv};
    REQUIRE(!ui.submit_ai_proposal(ai_action));
    REQUIRE(ui.snapshot().project_revision == before.project_revision);

    const auto imported = ui.import_engineering_bom_csv(csv_path);
    REQUIRE(imported);
    REQUIRE(imported.value().document_changed);
    auto after = ui.snapshot();
    REQUIRE(after.project_revision == before.project_revision.next());
    REQUIRE(after.engineering.source_count == 3U);
    REQUIRE(after.engineering.bom_count == 4U);
    REQUIRE(after.engineering.bom.size() == 4U);
    REQUIRE(after.engineering.bom.at(2U).source_id == 3U);
    REQUIRE(after.engineering.bom.at(2U).reconciliation == "exact");
    REQUIRE(after.engineering.bom.at(2U).mapped_part_id == 10U);
    REQUIRE(after.engineering.bom.at(2U).assembly_quantity == 2.0);
    REQUIRE(after.operations.back().category == carto::ui::OperationCategory::provider);
    REQUIRE(after.operations.back().action == "Import and Reconcile Engineering BOM CSV");

    const auto idempotent = ui.import_engineering_bom_csv(csv_path);
    REQUIRE(idempotent);
    REQUIRE(!idempotent.value().document_changed);
    REQUIRE(ui.snapshot().project_revision == after.project_revision);

    const auto stale = carto::application::HumanApplicationAccess::dispatch(
        session, ai_action);
    REQUIRE(!stale);
    REQUIRE(session.snapshot().project_revision == after.project_revision);

    const auto invalid_path = root / "invalid.csv";
    {
        std::ofstream stream(invalid_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << "part_number,description,revision,material,quantity,unit\n"
            "BRACKET-A17,Bracket,C,7075-T6,2,each\n";
    }
    const auto before_invalid = ui.snapshot();
    REQUIRE(!ui.import_engineering_bom_csv(invalid_path));
    REQUIRE(ui.snapshot().project_revision == before_invalid.project_revision);
    REQUIRE(ui.snapshot().engineering.digest == before_invalid.engineering.digest);

    const std::string replacement_csv =
        "part_number,revision,description,material,quantity,unit\n"
        "BRACKET-A17,C,Bracket,7075-T6,2,each\n"
        "UNLISTED-PART,A,Unlisted,,1,each\n";
    {
        std::ofstream stream(csv_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << replacement_csv;
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_engineering_bom_csv(csv_path));
    after = ui.snapshot();
    REQUIRE(after.engineering.source_count == 4U);
    REQUIRE(after.engineering.bom_count == 7U);
    const auto old_rows = std::count_if(after.engineering.bom.begin(),
        after.engineering.bom.end(), [](const auto& line) {
            return line.source_id == 3U && line.reconciliation == "unresolved";
        });
    REQUIRE(old_rows == 2U);
    REQUIRE(std::any_of(after.engineering.bom.begin(), after.engineering.bom.end(),
        [](const auto& line) {
            return line.source_id == 4U && line.reconciliation == "source-only";
        }));
    REQUIRE(std::any_of(after.engineering.bom.begin(), after.engineering.bom.end(),
        [](const auto& line) {
            return line.source_id == 4U && line.reconciliation == "assembly-only";
        }));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    auto undone = ui.snapshot();
    REQUIRE(undone.engineering.bom_count == 4U);
    REQUIRE(std::all_of(undone.engineering.bom.begin(), undone.engineering.bom.end(),
        [](const auto& line) {
            return line.source_id != 3U || line.reconciliation == "exact";
        }));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().engineering.bom_count == 7U);

    const auto project_path = root / "bom-import.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened.snapshot().engineering.bom_count == 7U);
    REQUIRE(reopened.snapshot().engineering.digest == ui.snapshot().engineering.digest);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_imports_exports_and_reconciles_engineering_authoring_files() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-authoring-io-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    const auto source_path = root / "source.carto-engineering-authoring";
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << fixture.value().serialize();
        REQUIRE(stream.good());
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Engineering file import"}));
    const auto imported = ui.import_engineering_authoring(source_path);
    REQUIRE(imported);
    REQUIRE(imported.value().document_changed);
    REQUIRE(ui.snapshot().engineering.digest == fixture.value().canonical_digest().value());
    REQUIRE(ui.snapshot().engineering.sources.size() == fixture.value().sources.size());
    REQUIRE(ui.snapshot().operations.back().category == carto::ui::OperationCategory::provider);
    REQUIRE(ui.snapshot().undo_count > 0U);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(!ui.snapshot().engineering.present);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().engineering.present);

    const auto exported_path = root / "exported.carto-engineering-authoring";
    REQUIRE(ui.export_engineering_authoring(exported_path));
    std::ifstream exported_stream(exported_path, std::ios::binary);
    REQUIRE(exported_stream.good());
    const std::string exported_bytes((std::istreambuf_iterator<char>(exported_stream)), {});
    REQUIRE(carto::engineering::AuthoringDocument::deserialize(exported_bytes));
    REQUIRE(exported_bytes == fixture.value().serialize());

    const auto invalid_path = root / "invalid.carto-engineering-authoring";
    {
        std::ofstream stream(invalid_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << "not an engineering authoring document";
    }
    const auto revision_before_invalid_import = ui.snapshot().project_revision;
    const auto digest_before_invalid_import = ui.snapshot().engineering.digest;
    REQUIRE(!ui.import_engineering_authoring(invalid_path));
    REQUIRE(ui.snapshot().project_revision == revision_before_invalid_import);
    REQUIRE(ui.snapshot().engineering.digest == digest_before_invalid_import);
    REQUIRE(!ui.submit_ai_proposal(carto::application::ImportEngineeringAuthoringAction{
        fixture.value().serialize()}));
    REQUIRE(ui.snapshot().project_revision == revision_before_invalid_import);

    const auto oversized_path = root / "oversized.carto-engineering-authoring";
    {
        std::ofstream stream(oversized_path, std::ios::binary);
        REQUIRE(stream.good());
    }
    std::filesystem::resize_file(
        oversized_path, carto::engineering::AuthoringDocument::kMaxSerializedBytes + 1U);
    REQUIRE(!ui.import_engineering_authoring(oversized_path));
    REQUIRE(ui.snapshot().project_revision == revision_before_invalid_import);

    const auto source = ui.snapshot().engineering.sources.front();
    REQUIRE(ui.invalidate_engineering_source(
        source.id, "revision-d", digest("revision-d-bytes")));
    REQUIRE(ui.snapshot().engineering.stale);
    REQUIRE(ui.snapshot().engineering.sources.front().revision == "revision-d");
    REQUIRE(ui.snapshot().engineering.sources.front().digest == digest("revision-d-bytes"));
    REQUIRE(!ui.snapshot().engineering.publication_ready);
    REQUIRE(!ui.publish_engineering_package(root / "blocked.carto-inspection"));

    auto reconciled = fixture.value();
    reconciled.project_revision = reconciled.project_revision.next();
    {
        std::ofstream stream(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << reconciled.serialize();
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_engineering_authoring(source_path));
    REQUIRE(!ui.snapshot().engineering.stale);
    REQUIRE(ui.snapshot().engineering.vanta_contract_admissible);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void source_invalidation_preserves_authored_records_and_stales_products() {
    auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    const auto old_requirement = fixture.value().requirements.at(1U);
    const auto result = fixture.value().invalidate_source(
        1U, "D", digest("revision-d"));
    REQUIRE(result);
    REQUIRE(fixture.value().requirements.at(1U).id == old_requirement.id);
    REQUIRE(fixture.value().requirements.at(1U).extraction ==
            carto::engineering::ExtractionState::stale);
    REQUIRE(fixture.value().targets.at(0U).status == carto::engineering::TargetStatus::stale);
    REQUIRE(fixture.value().path->status == carto::engineering::DerivedProductStatus::stale);
    REQUIRE(fixture.value().derived_products.at(0U).status ==
            carto::engineering::DerivedProductStatus::stale);
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        fixture.value(), fixture.value().project_revision, fixture.value().project_namespace));

    auto bom_source = carto::engineering::make_dv012_fixture();
    REQUIRE(bom_source);
    REQUIRE(bom_source.value().invalidate_source(2U, "D", digest("bom-revision-d")));
    REQUIRE(std::all_of(bom_source.value().bom.begin(), bom_source.value().bom.end(),
        [](const auto& line) {
            return line.source_id != 2U || line.reconciliation ==
                carto::engineering::ReconciliationStatus::unresolved;
        }));
}

void step_boundary_never_fakes_a_cad_import() {
    carto::engineering::StepAp242Provider provider;
    const auto probe = provider.probe("ISO-10303-21; HEADER; FILE_SCHEMA(('AP242')); ENDSEC;");
    REQUIRE(probe);
    REQUIRE(probe.value().recognized);
    REQUIRE(!probe.value().supported);
    REQUIRE(!provider.read_assembly(
        "ISO-10303-21;", "fixture.step", "C",
        digest("settings")));
}

} // namespace

int main() {
    try {
        fixture_round_trips_with_identity_and_provenance();
        bom_csv_parser_and_reconciliation_fail_closed_on_ambiguous_evidence();
        publication_and_vanta_admission_bind_exact_bytes();
        manual_requirement_authoring_is_revision_bound_and_auditable();
        inspection_characteristics_require_current_human_verified_contracts();
        inspection_target_authoring_is_human_revision_and_workcell_bound();
        inspection_target_review_is_human_revision_bound_and_preserves_decisions();
        source_invalidation_preserves_authored_records_and_stales_products();
        step_boundary_never_fakes_a_cad_import();
        application_wires_engineering_authoring_through_persistence_and_history();
        ui_exposes_engineering_workspace_and_immutable_publication();
        ui_imports_bom_csv_through_revision_bound_journal_and_preserves_source_history();
        ui_imports_exports_and_reconciles_engineering_authoring_files();
        std::cout << "engineering tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
