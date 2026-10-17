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
#include <sstream>
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

void requirement_csv_parser_accepts_only_the_declared_dimensional_profile() {
    const std::string csv =
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\r\n"
        "34,\"Section 5\",DIA-04,diameter,FACE-P04-END,5.0,0.02,0.02,millimeters,\"within bilateral tolerance\",\"Diameter 5.0, +/- 0.02 mm\"\r\n";
    const auto parsed = carto::engineering::parse_requirement_csv(csv);
    REQUIRE(parsed);
    REQUIRE(parsed.value().size() == 1U);
    REQUIRE(parsed.value().front().page == 34U);
    REQUIRE(parsed.value().front().section == "Section 5");
    REQUIRE(parsed.value().front().kind == carto::engineering::RequirementKind::diameter);
    REQUIRE(parsed.value().front().topology_external_id == "FACE-P04-END");
    REQUIRE(parsed.value().front().nominal == 5.0);
    REQUIRE(parsed.value().front().original_text == "Diameter 5.0, +/- 0.02 mm");

    REQUIRE(!carto::engineering::parse_requirement_csv(
        "callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "DIA-04,diameter,FACE-P04-END,5,0.02,0.02,millimeters,pass,Diameter 5\n"));
    REQUIRE(!carto::engineering::parse_requirement_csv(
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "34,S,DIA-04,position,FACE-P04-END,5,0,0,millimeters,pass,Position\n"));
    REQUIRE(!carto::engineering::parse_requirement_csv(
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "34,S,DIA-04,diameter,FACE-P04-END,NaN,0,0,millimeters,pass,Diameter\n"));
    REQUIRE(!carto::engineering::parse_requirement_csv(
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "34,S,DIA-04,diameter,FACE-P04-END,5,0,0,centimeters,pass,Diameter\n"));
    REQUIRE(!carto::engineering::parse_requirement_csv(
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "34,S,DIA-04,angle,FACE-P04-END,359,2,0,degrees,pass,Angle\n"));
    REQUIRE(!carto::engineering::parse_requirement_csv(
        std::string(carto::engineering::kMaxRequirementCsvBytes + 1U, 'x')));
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

    auto stale_path = fixture.value();
    stale_path.path->status = carto::engineering::DerivedProductStatus::stale;
    for (auto& sample : stale_path.path->samples) {
        sample.status = carto::engineering::PathSampleStatus::stale;
    }
    REQUIRE(stale_path.validate());
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        stale_path, stale_path.project_revision, stale_path.project_namespace));

    auto stale_derived_product = fixture.value();
    stale_derived_product.derived_products.front().status =
        carto::engineering::DerivedProductStatus::stale;
    stale_derived_product.derived_products.front().stale_reason =
        "workcell revision changed";
    REQUIRE(stale_derived_product.validate());
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        stale_derived_product, stale_derived_product.project_revision,
        stale_derived_product.project_namespace));

    const auto admission_for_stale_package = [&](auto mutate, std::string_view expected_reason) {
        auto tampered_authoring = carto::engineering::AuthoringDocument::deserialize(
            package.value().authoring_bytes);
        REQUIRE(tampered_authoring);
        mutate(tampered_authoring.value());
        auto stale_package = package.value();
        stale_package.authoring_bytes = tampered_authoring.value().serialize();
        REQUIRE(stale_package.refresh_digest());
        const auto stale_admission = carto::engineering::admit_for_vanta(stale_package);
        REQUIRE(stale_admission);
        REQUIRE(stale_admission.value().status ==
            carto::engineering::VantaAdmissionStatus::refuse);
        REQUIRE(std::find(stale_admission.value().reasons.begin(),
            stale_admission.value().reasons.end(), expected_reason) !=
            stale_admission.value().reasons.end());
    };
    admission_for_stale_package([](auto& authoring) {
        authoring.path->status = carto::engineering::DerivedProductStatus::stale;
        for (auto& sample : authoring.path->samples) {
            sample.status = carto::engineering::PathSampleStatus::stale;
        }
    }, "inspection path is stale");
    admission_for_stale_package([](auto& authoring) {
        authoring.derived_products.front().status =
            carto::engineering::DerivedProductStatus::stale;
        authoring.derived_products.front().stale_reason = "source revision changed";
    }, "derived engineering product 701 is stale");

    auto local_source_path = fixture.value();
    const char windows_separator = '\\';
    local_source_path.sources.front().locator = std::string("C:") + windows_separator +
        "Users" + windows_separator + "operator" + windows_separator +
        "source" + windows_separator + "assembly.step";
    REQUIRE(local_source_path.validate());
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        local_source_path, local_source_path.project_revision,
        local_source_path.project_namespace));

    auto local_posix_path = fixture.value();
    local_posix_path.sources.front().locator =
        std::string(1U, '/') + "home" + "/operator/source/assembly.step";
    REQUIRE(local_posix_path.validate());
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        local_posix_path, local_posix_path.project_revision,
        local_posix_path.project_namespace));

    auto local_workcell_path = fixture.value();
    local_workcell_path.workcell->robot_source_identity =
        std::string("file:///C:") + "/Users/operator/source/robot.urdf";
    REQUIRE(local_workcell_path.validate());
    REQUIRE(!carto::engineering::InspectionPackage::publish(
        local_workcell_path, local_workcell_path.project_revision,
        local_workcell_path.project_namespace));

    auto external_uri = fixture.value();
    external_uri.sources.front().locator = "https://engineering.example/assembly.step";
    REQUIRE(carto::engineering::InspectionPackage::publish(
        external_uri, external_uri.project_revision, external_uri.project_namespace));

    auto rehashed_local_path_package = package.value();
    auto disclosed_authoring = carto::engineering::AuthoringDocument::deserialize(
        rehashed_local_path_package.authoring_bytes);
    REQUIRE(disclosed_authoring);
    disclosed_authoring.value().workcell->frames.front().source =
        "\\\\private-server\\engineering\\datum-a.frame";
    rehashed_local_path_package.authoring_bytes = disclosed_authoring.value().serialize();
    REQUIRE(rehashed_local_path_package.refresh_digest());
    REQUIRE(!rehashed_local_path_package.validate());

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
        "within bilateral tolerance", true, {}});
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

    auto changed_basis = authored.value();
    auto imported_target = std::find_if(changed_basis.targets.begin(),
        changed_basis.targets.end(), [](const auto& value) {
            return value.id == 504U;
        });
    REQUIRE(imported_target != changed_basis.targets.end());
    const auto revision_marker = imported_target->generation_basis.find("workcell-revision=C");
    REQUIRE(revision_marker != std::string::npos);
    imported_target->generation_basis.replace(revision_marker,
        std::string("workcell-revision=C").size(), "workcell-revision=B");
    REQUIRE(ui.dispatch(carto::application::ImportEngineeringAuthoringAction{
        changed_basis.serialize()}));
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

void workcell_edits_are_revisioned_audited_and_invalidate_targets() {
    auto fixture = carto::engineering::make_dv012_fixture();
    REQUIRE(fixture);
    const std::string current_header = "CARTOGRAPHER_ENGINEERING_AUTHORING 3\n";
    const std::string legacy_header = "CARTOGRAPHER_ENGINEERING_AUTHORING 1\n";
    const auto downgrade = [](std::string bytes, std::string_view header) {
        const std::string current = "CARTOGRAPHER_ENGINEERING_AUTHORING 3\n";
        REQUIRE(bytes.starts_with(current));
        bytes.replace(0U, current.size(), header);
        std::istringstream input(bytes);
        std::ostringstream output;
        std::string line;
        while (std::getline(input, line)) {
            if (line.starts_with("REQUIREMENT ") && line.ends_with(" \"\"")) {
                line.resize(line.size() - 3U);
            }
            output << line << '\n';
        }
        return output.str();
    };
    std::string legacy_bytes = downgrade(
        fixture.value().serialize(), legacy_header);
    const std::string empty_history = "WORKCELL_REVISION_HISTORY 0\n";
    const auto history_offset = legacy_bytes.find(empty_history);
    REQUIRE(history_offset != std::string::npos);
    legacy_bytes.erase(history_offset, empty_history.size());
    const auto migrated = carto::engineering::AuthoringDocument::deserialize(legacy_bytes);
    REQUIRE(migrated);
    REQUIRE(migrated.value().workcell_revision_history.empty());
    REQUIRE(migrated.value().serialize().starts_with(current_header));
    const auto schema_two = carto::engineering::AuthoringDocument::deserialize(
        downgrade(fixture.value().serialize(),
            "CARTOGRAPHER_ENGINEERING_AUTHORING 2\n"));
    REQUIRE(schema_two);
    REQUIRE(schema_two.value().serialize().starts_with(current_header));

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Workcell edits"}));
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportEngineeringFixtureAction{}));
    carto::ui::UiController ui(session);

    const auto make_entity_action = [](const carto::application::ApplicationSnapshot& snapshot,
                                       std::string revision) {
        carto::application::UpdateEngineeringWorkcellAction action;
        action.expected_project_revision = snapshot.project_revision;
        action.expected_authoring_revision = snapshot.engineering.authored_revision;
        action.expected_workcell_revision = snapshot.engineering.workcell->revision;
        action.new_workcell_revision = std::move(revision);
        action.entity_id = 603U;
        action.transform = {{0.2, 0.0, 0.0}, carto::core::Quaternion::identity(),
            {1.0, 1.0, 1.0}};
        action.justification = "Operator-entered robot base placement from station setup sheet";
        return action;
    };
    const auto before = session.snapshot();
    REQUIRE(before.engineering.workcell.has_value());
    const auto valid_action = make_entity_action(before, "D");
    REQUIRE(!ui.submit_ai_proposal(valid_action));

    const auto before_authoring = session.engineering_authoring();
    REQUIRE(before_authoring);
    auto unrevisioned_import = before_authoring.value();
    unrevisioned_import.workcell->entities.at(2U).transform.translation.x = 9.0;
    REQUIRE(!ui.dispatch(carto::application::ImportEngineeringAuthoringAction{
        unrevisioned_import.serialize()}));

    auto stale_workcell = valid_action;
    stale_workcell.expected_workcell_revision = "B";
    REQUIRE(!ui.dispatch(stale_workcell));
    auto same_revision = valid_action;
    same_revision.new_workcell_revision = "C";
    REQUIRE(!ui.dispatch(same_revision));
    auto missing_justification = valid_action;
    missing_justification.justification.clear();
    REQUIRE(!ui.dispatch(missing_justification));
    auto ambiguous_target = valid_action;
    ambiguous_target.keep_out_id = 610U;
    REQUIRE(!ui.dispatch(ambiguous_target));
    auto scaled_transform = valid_action;
    scaled_transform.transform.scale = {1.0, 1.5, 1.0};
    REQUIRE(!ui.dispatch(scaled_transform));
    auto invalid_keep_out = valid_action;
    invalid_keep_out.entity_id.reset();
    invalid_keep_out.keep_out_id = 610U;
    invalid_keep_out.dimensions = {-1.0, 0.5, 0.25};
    REQUIRE(!ui.dispatch(invalid_keep_out));
    REQUIRE(session.snapshot().project_revision == before.project_revision);

    const auto updated = ui.update_engineering_workcell(valid_action);
    REQUIRE(updated && updated.value().document_changed);
    const auto after_update = session.snapshot();
    REQUIRE(after_update.project_revision == before.project_revision.next());
    REQUIRE(after_update.engineering.workcell->revision == "D");
    REQUIRE(after_update.engineering.workcell->revision_history.size() == 1U);
    REQUIRE(after_update.engineering.workcell->revision_history.front().previous_revision == "C");
    REQUIRE(after_update.engineering.workcell->revision_history.front().subject_kind == "entity");
    REQUIRE(after_update.engineering.workcell->revision_history.front().subject_id == 603U);
    REQUIRE(after_update.engineering.workcell->revision_history.front().justification ==
        valid_action.justification);
    const auto authored = session.engineering_authoring();
    REQUIRE(authored);
    const auto robot_base = std::find_if(authored.value().workcell->entities.begin(),
        authored.value().workcell->entities.end(), [](const auto& value) {
            return value.id == 603U;
        });
    REQUIRE(robot_base != authored.value().workcell->entities.end());
    REQUIRE(robot_base->transform.translation.x == 0.2);
    REQUIRE(robot_base->source == "fixture://dv012/robot");
    REQUIRE(authored.value().targets.at(0U).status == carto::engineering::TargetStatus::stale);
    REQUIRE(authored.value().targets.at(1U).status == carto::engineering::TargetStatus::rejected);
    REQUIRE(authored.value().path->status == carto::engineering::DerivedProductStatus::stale);
    REQUIRE(authored.value().derived_products.front().stale_reason.find("workcell revision") !=
        std::string::npos);
    REQUIRE(!after_update.engineering.publication_ready);
    REQUIRE(ui.snapshot().operations.back().action == "Update Workcell Entity");
    REQUIRE(ui.snapshot().operations.back().category ==
        carto::ui::OperationCategory::committed_command);
    const auto reopened = carto::engineering::AuthoringDocument::deserialize(
        authored.value().serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().workcell_revision_history ==
        authored.value().workcell_revision_history);
    auto malformed_history = authored.value();
    malformed_history.workcell_revision_history.front().subject_id = 99999U;
    REQUIRE(!malformed_history.validate());
    auto changed_under_revision = authored.value();
    changed_under_revision.workcell->entities.at(2U).transform.translation.x = 0.3;
    REQUIRE(!changed_under_revision.validate());

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().engineering.workcell->revision == "C");
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().engineering.workcell->revision == "D");

    auto keep_out_action = valid_action;
    const auto before_keep_out = session.snapshot();
    keep_out_action.expected_project_revision = before_keep_out.project_revision;
    keep_out_action.expected_authoring_revision = before_keep_out.engineering.authored_revision;
    keep_out_action.expected_workcell_revision = "D";
    keep_out_action.new_workcell_revision = "E";
    keep_out_action.entity_id.reset();
    keep_out_action.keep_out_id = 610U;
    keep_out_action.transform = carto::core::Transform::identity();
    keep_out_action.dimensions = {0.6, 0.6, 0.3};
    keep_out_action.justification = "Operator-entered fixture exclusion extent from setup review";
    REQUIRE(ui.update_engineering_workcell(keep_out_action));
    const auto after_keep_out = session.engineering_authoring();
    REQUIRE(after_keep_out);
    REQUIRE(after_keep_out.value().workcell_revision_history.size() == 2U);
    REQUIRE(after_keep_out.value().workcell_revision_history.back().previous_revision == "D");
    REQUIRE(after_keep_out.value().workcell->keep_outs.front().dimensions.x == 0.6);

    const auto before_create = session.snapshot();
    carto::application::CreateEngineeringKeepOutAction create_keep_out;
    create_keep_out.expected_project_revision = before_create.project_revision;
    create_keep_out.expected_authoring_revision = before_create.engineering.authored_revision;
    create_keep_out.expected_workcell_revision = before_create.engineering.workcell->revision;
    create_keep_out.new_workcell_revision = "F";
    create_keep_out.purpose = "Operator exclusion volume";
    create_keep_out.center = {1.0, 0.25, 0.5};
    create_keep_out.dimensions = {1.2, 0.8, 1.7};
    create_keep_out.justification = "Operator-entered exclusion from station setup review";
    REQUIRE(!ui.submit_ai_proposal(create_keep_out));
    auto stale_workcell_create = create_keep_out;
    stale_workcell_create.expected_workcell_revision = "D";
    REQUIRE(!ui.dispatch(stale_workcell_create));
    auto stale_authoring_create = create_keep_out;
    stale_authoring_create.expected_authoring_revision =
        create_keep_out.expected_authoring_revision.next();
    REQUIRE(!ui.dispatch(stale_authoring_create));
    auto missing_reason_create = create_keep_out;
    missing_reason_create.justification.clear();
    REQUIRE(!ui.dispatch(missing_reason_create));
    auto multiline_reason_create = create_keep_out;
    multiline_reason_create.justification = "Operator-entered reason\nsecond line";
    REQUIRE(!ui.dispatch(multiline_reason_create));
    auto invalid_dimensions_create = create_keep_out;
    invalid_dimensions_create.dimensions.y = 0.0;
    REQUIRE(!ui.dispatch(invalid_dimensions_create));
    auto invalid_purpose_create = create_keep_out;
    invalid_purpose_create.purpose = "  ";
    REQUIRE(!ui.dispatch(invalid_purpose_create));
    REQUIRE(session.snapshot().project_revision == before_create.project_revision);

    const auto created = ui.create_engineering_keep_out(create_keep_out);
    REQUIRE(created && created.value().document_changed);
    const auto after_create = session.snapshot();
    REQUIRE(after_create.project_revision == before_create.project_revision.next());
    REQUIRE(after_create.engineering.workcell->revision == "F");
    REQUIRE(after_create.engineering.workcell->keep_outs.size() == 2U);
    const auto& authored_keep_out = after_create.engineering.workcell->keep_outs.back();
    REQUIRE(authored_keep_out.id == 621U);
    REQUIRE(authored_keep_out.kind == "box");
    REQUIRE(authored_keep_out.purpose == create_keep_out.purpose);
    REQUIRE(authored_keep_out.source == "manual://cartographer/workcell");
    REQUIRE(authored_keep_out.transform.translation.x == create_keep_out.center.x);
    REQUIRE(authored_keep_out.dimensions.z == create_keep_out.dimensions.z);
    REQUIRE(after_create.engineering.workcell->revision_history.size() == 3U);
    const auto& creation_record = after_create.engineering.workcell->revision_history.back();
    REQUIRE(creation_record.previous_revision == "E");
    REQUIRE(creation_record.subject_kind == "keep-out");
    REQUIRE(creation_record.subject_id == authored_keep_out.id);
    REQUIRE(creation_record.justification == create_keep_out.justification);
    const auto created_authoring = session.engineering_authoring();
    REQUIRE(created_authoring && created_authoring.value().validate());
    const auto created_roundtrip = carto::engineering::AuthoringDocument::deserialize(
        created_authoring.value().serialize());
    REQUIRE(created_roundtrip);
    REQUIRE(created_roundtrip.value().workcell_revision_history ==
        created_authoring.value().workcell_revision_history);
    REQUIRE(created_roundtrip.value().workcell->keep_outs.back().id == authored_keep_out.id);
    REQUIRE(created_authoring.value().targets.at(0U).status ==
        carto::engineering::TargetStatus::stale);
    REQUIRE(created_authoring.value().targets.at(1U).status ==
        carto::engineering::TargetStatus::rejected);
    REQUIRE(created_authoring.value().path->status ==
        carto::engineering::DerivedProductStatus::stale);
    REQUIRE(!after_create.engineering.publication_ready);
    REQUIRE(ui.snapshot().operations.back().action == "Create Workcell Keep-Out");
    REQUIRE(ui.snapshot().operations.back().category ==
        carto::ui::OperationCategory::committed_command);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().engineering.workcell->revision == "E");
    REQUIRE(session.snapshot().engineering.workcell->keep_outs.size() == 1U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().engineering.workcell->revision == "F");
    REQUIRE(session.snapshot().engineering.workcell->keep_outs.size() == 2U);

    const auto before_frame = session.snapshot();
    carto::application::CreateEngineeringWorkcellFrameAction create_frame;
    create_frame.expected_project_revision = before_frame.project_revision;
    create_frame.expected_authoring_revision = before_frame.engineering.authored_revision;
    create_frame.expected_workcell_revision = before_frame.engineering.workcell->revision;
    create_frame.new_workcell_revision = "G";
    create_frame.name = "Sensor inspection datum";
    create_frame.transform = {{0.2, -0.3, 0.4}, {0.0, 0.0, 0.7071067811865475, 0.7071067811865476},
        {1.0, 1.0, 1.0}};
    create_frame.justification = "Operator-entered datum pose from station setup sheet";
    REQUIRE(!ui.submit_ai_proposal(create_frame));
    auto stale_frame = create_frame;
    stale_frame.expected_workcell_revision = "E";
    REQUIRE(!ui.dispatch(stale_frame));
    auto missing_frame_reason = create_frame;
    missing_frame_reason.justification.clear();
    REQUIRE(!ui.dispatch(missing_frame_reason));
    auto multiline_frame_name = create_frame;
    multiline_frame_name.name = "Datum\nA";
    REQUIRE(!ui.dispatch(multiline_frame_name));
    auto invalid_frame_rotation = create_frame;
    invalid_frame_rotation.transform.rotation = {0.0, 0.0, 0.0, 0.0};
    REQUIRE(!ui.dispatch(invalid_frame_rotation));
    auto scaled_frame = create_frame;
    scaled_frame.transform.scale = {1.0, 1.1, 1.0};
    REQUIRE(!ui.dispatch(scaled_frame));
    REQUIRE(session.snapshot().project_revision == before_frame.project_revision);

    const auto created_frame = ui.create_engineering_workcell_frame(create_frame);
    REQUIRE(created_frame && created_frame.value().document_changed);
    const auto after_frame = session.snapshot();
    REQUIRE(after_frame.engineering.workcell->revision == "G");
    REQUIRE(after_frame.engineering.workcell->revision_history.size() == 4U);
    const auto frame_authoring = session.engineering_authoring();
    REQUIRE(frame_authoring && frame_authoring.value().validate());
    const auto frame_roundtrip = carto::engineering::AuthoringDocument::deserialize(
        frame_authoring.value().serialize());
    REQUIRE(frame_roundtrip);
    REQUIRE(frame_roundtrip.value().workcell->frames.size() == 2U);
    REQUIRE(frame_roundtrip.value().workcell_revision_history.back().subject_kind == "frame");
    const auto& authored_frame = frame_authoring.value().workcell->frames.back();
    REQUIRE(authored_frame.id == 622U);
    REQUIRE(authored_frame.kind == carto::engineering::WorkcellEntityKind::reference_frame);
    REQUIRE(authored_frame.name == create_frame.name);
    REQUIRE(authored_frame.source == "manual://cartographer/workcell");
    REQUIRE(std::abs(authored_frame.transform.rotation.z - 0.7071067811865475) < 1e-12);
    const auto& frame_record = frame_authoring.value().workcell_revision_history.back();
    REQUIRE(frame_record.previous_revision == "F");
    REQUIRE(frame_record.subject_kind == "frame");
    REQUIRE(frame_record.subject_id == authored_frame.id);
    REQUIRE(frame_record.justification == create_frame.justification);
    REQUIRE(std::any_of(after_frame.engineering.workcell->entities.begin(),
        after_frame.engineering.workcell->entities.end(), [](const auto& value) {
            return value.reference_frame && value.name == "Sensor inspection datum";
        }));
    REQUIRE(!after_frame.engineering.publication_ready);
    REQUIRE(ui.snapshot().operations.back().action == "Create Workcell Reference Frame");
    REQUIRE(ui.snapshot().operations.back().category ==
        carto::ui::OperationCategory::committed_command);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::UndoAction{}));
    REQUIRE(session.snapshot().engineering.workcell->revision == "F");
    REQUIRE(session.engineering_authoring().value().workcell->frames.size() == 1U);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::RedoAction{}));
    REQUIRE(session.snapshot().engineering.workcell->revision == "G");
    REQUIRE(session.engineering_authoring().value().workcell->frames.size() == 2U);
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
    REQUIRE(snapshot.engineering.path.has_value());
    REQUIRE(snapshot.engineering.path->status == "unverified");
    REQUIRE(snapshot.engineering.path->samples.size() == 2U);
    REQUIRE(snapshot.engineering.path->samples.front().status == "unverified");
    REQUIRE(snapshot.engineering.path->samples.front().target_id == 500U);

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
    const auto imported_authoring = session.engineering_authoring();
    REQUIRE(imported_authoring);
    REQUIRE(imported_authoring.value().sources.back().locator == csv_path.filename().generic_string());
    REQUIRE(imported_authoring.value().sources.back().locator.find(root.generic_string()) ==
        std::string::npos);
    auto unsafe_locator = carto::application::ImportEngineeringBomCsvAction{
        after.project_revision,
        after.engineering.authored_revision,
        csv_path.generic_string(),
        initial_csv};
    REQUIRE(!carto::application::HumanApplicationAccess::dispatch(session, unsafe_locator));
    REQUIRE(session.snapshot().project_revision == after.project_revision);

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

void ui_imports_and_reviews_structured_requirements_without_promoting_proposals() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-requirement-csv-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto csv_path = root / "requirements.csv";
    const std::string initial_csv =
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "34,\"Section 5\",DIA-04,diameter,FACE-P04-END,5.0,0.02,0.02,millimeters,\"within bilateral tolerance\",\"Diameter 5.0 +/- 0.02 mm\"\n"
        "35,\"Section 6\",DIST-07,dimensional-distance,EDGE-A17-DATUM,12.0,0.05,0.05,millimeters,\"within bilateral tolerance\",\"12.00 +/- 0.05 mm between datums\"\n"
        "36,\"Section 7\",DIA-08,diameter,FACE-P04-END,3.0,0.01,0.01,millimeters,\"within bilateral tolerance\",\"Diameter 3.00 +/- 0.01 mm\"\n";

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Requirement CSV import"}));
    REQUIRE(ui.import_engineering_fixture());

    auto ambiguous_authoring = session.engineering_authoring();
    REQUIRE(ambiguous_authoring);
    ambiguous_authoring.value().topology.push_back({
        103U, 1U, 10U, 20U, carto::engineering::TopologyKind::face,
        "FACE-A17-OUTER", "C", 1000U, 14U});
    REQUIRE(ambiguous_authoring.value().validate());
    REQUIRE(ui.dispatch(carto::application::ImportEngineeringAuthoringAction{
        ambiguous_authoring.value().serialize()}));

    const std::string ambiguous_csv =
        "page,section,callout,kind,topology_external_id,nominal,plus,minus,unit,acceptance_rule,original_text\n"
        "34,S,DIA-AMBIGUOUS,diameter,FACE-A17-OUTER,5,0.02,0.02,millimeters,pass,Diameter 5\n";
    {
        std::ofstream stream(csv_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << ambiguous_csv;
        REQUIRE(stream.good());
    }
    const auto before_ambiguous = ui.snapshot();
    REQUIRE(!ui.import_engineering_requirement_csv(csv_path));
    REQUIRE(ui.snapshot().project_revision == before_ambiguous.project_revision);
    REQUIRE(ui.snapshot().engineering.digest == before_ambiguous.engineering.digest);

    {
        std::ofstream stream(csv_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << initial_csv;
        REQUIRE(stream.good());
    }
    const auto before = ui.snapshot();
    const carto::application::ImportEngineeringRequirementCsvAction ai_action{
        before.project_revision, before.engineering.authored_revision,
        csv_path.filename().generic_string(), initial_csv};
    REQUIRE(!ui.submit_ai_proposal(ai_action));
    REQUIRE(ui.snapshot().project_revision == before.project_revision);

    const auto imported = ui.import_engineering_requirement_csv(csv_path);
    REQUIRE(imported);
    REQUIRE(imported.value().document_changed);
    auto after = ui.snapshot();
    REQUIRE(after.project_revision == before.project_revision.next());
    REQUIRE(after.engineering.source_count == 3U);
    REQUIRE(after.engineering.requirement_count == 6U);
    REQUIRE(after.operations.back().category == carto::ui::OperationCategory::provider);
    REQUIRE(after.operations.back().action == "Import Engineering Requirement CSV");
    const auto first_proposal = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [](const auto& requirement) {
            return requirement.callout == "DIA-04";
        });
    const auto second_proposal = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [](const auto& requirement) {
            return requirement.callout == "DIST-07";
        });
    const auto third_proposal = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [](const auto& requirement) {
            return requirement.callout == "DIA-08";
        });
    REQUIRE(first_proposal != after.engineering.requirements.end());
    REQUIRE(second_proposal != after.engineering.requirements.end());
    REQUIRE(third_proposal != after.engineering.requirements.end());
    const auto first_proposal_id = first_proposal->id;
    const auto second_proposal_id = second_proposal->id;
    const auto third_proposal_id = third_proposal->id;
    REQUIRE(first_proposal->extraction == "machine-extracted-proposed");
    REQUIRE(first_proposal->topology_ids == std::vector<std::uint64_t>{102U});
    REQUIRE(first_proposal->original_text == "Diameter 5.0 +/- 0.02 mm");
    REQUIRE(after.engineering.sources.back().locator == "requirements.csv");
    REQUIRE(after.engineering.sources.back().locator.find(root.generic_string()) ==
        std::string::npos);
    REQUIRE(!ui.build_engineering_package());

    carto::application::CreateEngineeringCharacteristicAction premature_characteristic;
    premature_characteristic.expected_project_revision = after.project_revision;
    premature_characteristic.expected_authoring_revision = after.engineering.authored_revision;
    premature_characteristic.requirement_id = first_proposal_id;
    premature_characteristic.kind = carto::engineering::InspectionKind::diameter;
    REQUIRE(!ui.dispatch(premature_characteristic));
    REQUIRE(ui.snapshot().project_revision == after.project_revision);

    carto::application::ReviewEngineeringRequirementAction missing_reason;
    missing_reason.expected_project_revision = after.project_revision;
    missing_reason.expected_authoring_revision = after.engineering.authored_revision;
    missing_reason.requirement_id = first_proposal_id;
    REQUIRE(!ui.dispatch(missing_reason));
    missing_reason.justification = "Operator verified against the source callout";
    REQUIRE(!ui.submit_ai_proposal(missing_reason));
    REQUIRE(ui.snapshot().project_revision == after.project_revision);

    auto correction = missing_reason;
    correction.decision = carto::application::RequirementReviewDecision::correct;
    correction.corrected_kind = carto::engineering::RequirementKind::diameter;
    correction.corrected_nominal = 5.1;
    correction.corrected_plus = 0.02;
    correction.corrected_minus = 0.02;
    correction.corrected_unit = "millimeters";
    correction.corrected_acceptance_rule = "within corrected bilateral tolerance";
    correction.justification = "Corrected the normalized nominal to match the cited drawing view";
    REQUIRE(ui.dispatch(correction));
    REQUIRE(ui.snapshot().operations.back().action == "Correct Engineering Requirement");

    after = ui.snapshot();
    carto::application::ReviewEngineeringRequirementAction verification;
    verification.expected_project_revision = after.project_revision;
    verification.expected_authoring_revision = after.engineering.authored_revision;
    verification.requirement_id = second_proposal_id;
    verification.decision = carto::application::RequirementReviewDecision::verify;
    verification.justification = "Compared the parsed distance and tolerance to the source text";
    REQUIRE(ui.dispatch(verification));
    REQUIRE(ui.snapshot().operations.back().action == "Verify Engineering Requirement");

    after = ui.snapshot();
    carto::application::ReviewEngineeringRequirementAction rejection;
    rejection.expected_project_revision = after.project_revision;
    rejection.expected_authoring_revision = after.engineering.authored_revision;
    rejection.requirement_id = third_proposal_id;
    rejection.decision = carto::application::RequirementReviewDecision::reject;
    rejection.justification = "This callout belongs to a different part and must not be applied";
    REQUIRE(ui.dispatch(rejection));
    REQUIRE(ui.snapshot().operations.back().action == "Reject Engineering Requirement");

    auto reviewed = session.engineering_authoring();
    REQUIRE(reviewed);
    const auto corrected = std::find_if(reviewed.value().requirements.begin(),
        reviewed.value().requirements.end(), [&first_proposal_id](const auto& requirement) {
        return requirement.id == first_proposal_id;
        });
    const auto verified = std::find_if(reviewed.value().requirements.begin(),
        reviewed.value().requirements.end(), [&second_proposal_id](const auto& requirement) {
        return requirement.id == second_proposal_id;
        });
    const auto rejected = std::find_if(reviewed.value().requirements.begin(),
        reviewed.value().requirements.end(), [&third_proposal_id](const auto& requirement) {
        return requirement.id == third_proposal_id;
        });
    REQUIRE(corrected != reviewed.value().requirements.end());
    REQUIRE(corrected->extraction == carto::engineering::ExtractionState::human_corrected);
    REQUIRE(corrected->nominal == 5.1);
    REQUIRE(corrected->provenance.original_text == "Diameter 5.0 +/- 0.02 mm");
    REQUIRE(corrected->human_review_justification == correction.justification);
    REQUIRE(verified != reviewed.value().requirements.end());
    REQUIRE(verified->extraction == carto::engineering::ExtractionState::human_verified);
    REQUIRE(rejected != reviewed.value().requirements.end());
    REQUIRE(rejected->extraction == carto::engineering::ExtractionState::rejected);
    REQUIRE(!rejected->enabled);

    auto current = ui.snapshot();
    carto::application::CreateEngineeringCharacteristicAction characteristic;
    characteristic.expected_project_revision = current.project_revision;
    characteristic.expected_authoring_revision = current.engineering.authored_revision;
    characteristic.requirement_id = corrected->id;
    characteristic.kind = carto::engineering::InspectionKind::diameter;
    characteristic.sensor.sensor_type = "laser-triangulation";
    characteristic.sensor.tool_center_frame = "TCP";
    characteristic.sensor.min_standoff = 0.1;
    characteristic.sensor.max_standoff = 0.5;
    characteristic.sensor.preferred_standoff = 0.25;
    characteristic.sensor.field_of_view_degrees = 30.0;
    characteristic.access.allowed_approach_directions = {"+z"};
    const auto created_characteristic = ui.dispatch(characteristic);
    if (!created_characteristic) {
        throw TestFailure(created_characteristic.error().message);
    }

    const auto idempotent_revision = ui.snapshot().project_revision;
    REQUIRE(ui.import_engineering_requirement_csv(csv_path));
    REQUIRE(ui.snapshot().project_revision == idempotent_revision);

    std::string replacement_csv = initial_csv;
    const auto old_value = replacement_csv.find("5.0,0.02,0.02");
    REQUIRE(old_value != std::string::npos);
    replacement_csv.replace(old_value, std::string("5.0,0.02,0.02").size(),
        "5.2,0.02,0.02");
    {
        std::ofstream stream(csv_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream << replacement_csv;
        REQUIRE(stream.good());
    }
    REQUIRE(ui.import_engineering_requirement_csv(csv_path));
    after = ui.snapshot();
    REQUIRE(after.engineering.source_count == 4U);
    const auto old_corrected = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [&first_proposal_id](const auto& requirement) {
        return requirement.id == first_proposal_id;
        });
    const auto old_rejected = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [&third_proposal_id](const auto& requirement) {
            return requirement.id == third_proposal_id;
        });
    REQUIRE(old_corrected != after.engineering.requirements.end());
    REQUIRE(old_corrected->extraction == "stale");
    REQUIRE(old_rejected != after.engineering.requirements.end());
    REQUIRE(old_rejected->extraction == "rejected");
    REQUIRE(!old_rejected->enabled);
    REQUIRE(std::any_of(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [](const auto& requirement) {
            return requirement.source_id == 4U &&
                requirement.extraction == "machine-extracted-proposed";
        }));

    const auto new_proposal = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [](const auto& requirement) {
            return requirement.source_id == 4U && requirement.callout == "DIA-04";
        });
    REQUIRE(new_proposal != after.engineering.requirements.end());
    const auto new_proposal_id = new_proposal->id;

    REQUIRE(ui.invalidate_engineering_source(1U, "D", digest("new STEP source revision")));
    after = ui.snapshot();
    carto::application::ReviewEngineeringRequirementAction stale_topology_review;
    stale_topology_review.expected_project_revision = after.project_revision;
    stale_topology_review.expected_authoring_revision = after.engineering.authored_revision;
    stale_topology_review.requirement_id = new_proposal_id;
    stale_topology_review.decision = carto::application::RequirementReviewDecision::verify;
    stale_topology_review.justification = "Attempt to review after CAD source changed";
    REQUIRE(!ui.dispatch(stale_topology_review));

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    after = ui.snapshot();
    const auto restored_proposal = std::find_if(after.engineering.requirements.begin(),
        after.engineering.requirements.end(), [&new_proposal_id](const auto& requirement) {
        return requirement.id == new_proposal_id;
        });
    REQUIRE(restored_proposal != after.engineering.requirements.end());
    carto::application::ReviewEngineeringRequirementAction restored_review;
    restored_review.expected_project_revision = after.project_revision;
    restored_review.expected_authoring_revision = after.engineering.authored_revision;
    restored_review.requirement_id = new_proposal_id;
    restored_review.decision = carto::application::RequirementReviewDecision::verify;
    restored_review.justification = "Verified after restoring the current CAD source revision";
    REQUIRE(ui.dispatch(restored_review));
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));

    const auto project_path = root / "requirements.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    const auto reopened_authoring = reopened.engineering_authoring();
    REQUIRE(reopened_authoring);
    const auto persisted_correction = std::find_if(
        reopened_authoring.value().requirements.begin(),
        reopened_authoring.value().requirements.end(), [&first_proposal_id](const auto& requirement) {
        return requirement.id == first_proposal_id;
        });
    REQUIRE(persisted_correction != reopened_authoring.value().requirements.end());
    REQUIRE(persisted_correction->human_review_justification == correction.justification);
    REQUIRE(reopened.snapshot().engineering.digest == ui.snapshot().engineering.digest);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_attaches_opaque_engineering_evidence_with_safe_provenance() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-evidence-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto source_path = root / "drawing.pdf";
    const std::string original_bytes("opaque evidence\0tail", 20U);
    {
        std::ofstream stream(source_path, std::ios::binary);
        REQUIRE(stream.good());
        stream.write(original_bytes.data(), static_cast<std::streamsize>(original_bytes.size()));
        REQUIRE(stream.good());
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Engineering evidence"}));
    REQUIRE(ui.import_engineering_fixture());
    const auto before = ui.snapshot();
    const carto::application::AttachEngineeringEvidenceAction ai_action{
        before.project_revision,
        before.engineering.authored_revision,
        carto::engineering::SourceKind::drawing,
        source_path.filename().generic_string(),
        original_bytes};
    REQUIRE(!ui.submit_ai_proposal(ai_action));
    REQUIRE(ui.snapshot().project_revision == before.project_revision);

    const auto attached = ui.attach_engineering_evidence(
        source_path, carto::engineering::SourceKind::drawing);
    REQUIRE(attached && attached.value().document_changed);
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.project_revision == before.project_revision.next());
    REQUIRE(snapshot.engineering.source_count == before.engineering.source_count + 1U);
    auto authoring = session.engineering_authoring();
    REQUIRE(authoring);
    const auto& source = authoring.value().sources.back();
    REQUIRE(source.kind == carto::engineering::SourceKind::drawing);
    REQUIRE(source.locator == "drawing.pdf");
    REQUIRE(source.locator.find(root.generic_string()) == std::string::npos);
    REQUIRE(source.digest == digest(original_bytes));
    REQUIRE(source.revision == "content-sha256:" + source.digest.hex());
    REQUIRE(source.diagnostics.size() == 2U);
    REQUIRE(source.diagnostics.front().find("no parsing") != std::string::npos);
    const auto serialized = authoring.value().serialize();
    REQUIRE(serialized.find(original_bytes) == std::string::npos);
    REQUIRE(serialized.find(root.generic_string()) == std::string::npos);
    REQUIRE(snapshot.operations.back().category == carto::ui::OperationCategory::provider);
    REQUIRE(snapshot.operations.back().action == "Attach Engineering Evidence");
    const auto source_id = source.id;

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().engineering.source_count == before.engineering.source_count);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.engineering.source_count == before.engineering.source_count + 1U);

    const auto same_source = ui.attach_engineering_evidence(
        source_path, carto::engineering::SourceKind::drawing);
    REQUIRE(same_source && !same_source.value().document_changed);
    REQUIRE(ui.snapshot().project_revision == snapshot.project_revision);

    carto::application::CreateEngineeringRequirementAction requirement;
    requirement.expected_project_revision = snapshot.project_revision;
    requirement.expected_authoring_revision = snapshot.engineering.authored_revision;
    requirement.source_id = source_id;
    requirement.topology_ids = {100U};
    requirement.nominal = 9.0;
    requirement.plus = 0.1;
    requirement.minus = 0.1;
    requirement.source_page = 34U;
    requirement.source_callout = "DIM-NEW";
    requirement.original_text = "9.00 +/- 0.10 mm";
    requirement.acceptance_rule = "within bilateral tolerance";
    REQUIRE(ui.dispatch(requirement));
    snapshot = ui.snapshot();
    authoring = session.engineering_authoring();
    REQUIRE(authoring);
    REQUIRE(authoring.value().requirements.back().provenance.source_id == source_id);

    const std::string replacement_bytes("revised drawing evidence");
    {
        std::ofstream stream(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream.write(replacement_bytes.data(),
            static_cast<std::streamsize>(replacement_bytes.size()));
        REQUIRE(stream.good());
    }
    const auto replaced = ui.attach_engineering_evidence(
        source_path, carto::engineering::SourceKind::drawing);
    REQUIRE(replaced && replaced.value().document_changed);
    const auto after_replace = ui.snapshot();
    REQUIRE(after_replace.engineering.source_count == snapshot.engineering.source_count + 1U);
    REQUIRE(after_replace.engineering.stale);
    authoring = session.engineering_authoring();
    REQUIRE(authoring);
    REQUIRE(authoring.value().sources[2U].digest == digest(original_bytes));
    REQUIRE(authoring.value().sources.back().digest == digest(replacement_bytes));
    REQUIRE(authoring.value().requirements.back().extraction ==
        carto::engineering::ExtractionState::stale);
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    authoring = session.engineering_authoring();
    REQUIRE(authoring);
    REQUIRE(authoring.value().sources.back().digest == digest(original_bytes));
    REQUIRE(authoring.value().requirements.back().extraction ==
        carto::engineering::ExtractionState::manual);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    authoring = session.engineering_authoring();
    REQUIRE(authoring);
    REQUIRE(authoring.value().sources.back().digest == digest(replacement_bytes));
    REQUIRE(authoring.value().requirements.back().extraction ==
        carto::engineering::ExtractionState::stale);

    auto stale_action = carto::application::AttachEngineeringEvidenceAction{
        snapshot.project_revision,
        snapshot.engineering.authored_revision,
        carto::engineering::SourceKind::specification,
        "spec.txt",
        "specification"};
    REQUIRE(!ui.dispatch(stale_action));
    const auto current = ui.snapshot();
    stale_action.expected_project_revision = current.project_revision;
    REQUIRE(!ui.dispatch(stale_action));
    stale_action.expected_authoring_revision = current.engineering.authored_revision;
    stale_action.locator = root.generic_string() + "\\private\\spec.txt";
    REQUIRE(!ui.dispatch(stale_action));
    stale_action.locator = "drawing.pdf";
    stale_action.kind = carto::engineering::SourceKind::step_ap242;
    REQUIRE(!ui.dispatch(stale_action));
    stale_action.kind = carto::engineering::SourceKind::specification;
    stale_action.source_bytes.resize(
        carto::engineering::kMaxEngineeringEvidenceBytes + 1U, 'x');
    REQUIRE(!ui.dispatch(stale_action));
    REQUIRE(ui.snapshot().project_revision == current.project_revision);

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
    REQUIRE(probe.value().schema_profile == "AP242");

    const auto lowercase_header = provider.probe(
        "iso-10303-21;\nheader;\nfile_schema(('ap242_managed_model_based_3d_engineering_mim_lf'));\n"
        "endsec;\ndata;\n");
    REQUIRE(lowercase_header);
    REQUIRE(lowercase_header.value().recognized);
    REQUIRE(!lowercase_header.value().supported);
    REQUIRE(lowercase_header.value().schema_profile ==
        "ap242_managed_model_based_3d_engineering_mim_lf");

    const auto unrelated_marker = provider.probe(
        "notes: ISO-10303-21; HEADER; FILE_SCHEMA(('AP242')); ENDSEC;");
    REQUIRE(unrelated_marker);
    REQUIRE(!unrelated_marker.value().recognized);
    REQUIRE(unrelated_marker.value().schema_profile == "unknown");

    const auto data_section_marker = provider.probe(
        "ISO-10303-21; HEADER; FILE_SCHEMA(('AP203')); ENDSEC; "
        "DATA; FILE_SCHEMA(('AP242')); ENDSEC;");
    REQUIRE(data_section_marker);
    REQUIRE(data_section_marker.value().recognized);
    REQUIRE(!data_section_marker.value().supported);
    REQUIRE(data_section_marker.value().schema_profile == "AP203");

    const auto description_marker = provider.probe(
        "ISO-10303-21; HEADER; FILE_DESCRIPTION(('AP242 evidence only'),'2;1'); "
        "FILE_SCHEMA(('AP203')); ENDSEC;");
    REQUIRE(description_marker);
    REQUIRE(description_marker.value().recognized);
    REQUIRE(!description_marker.value().supported);
    REQUIRE(description_marker.value().schema_profile == "AP203");

    const auto keyword_substring = provider.probe(
        "ISO-10303-21; NOTHEADER; FILE_SCHEMA(('AP242')); ENDSEC;");
    REQUIRE(keyword_substring);
    REQUIRE(!keyword_substring.value().recognized);

    const auto similar_schema_name = provider.probe(
        "ISO-10303-21; HEADER; FILE_SCHEMA(('NOT_AP242')); ENDSEC;");
    REQUIRE(similar_schema_name);
    REQUIRE(similar_schema_name.value().recognized);
    REQUIRE(!similar_schema_name.value().supported);
    REQUIRE(similar_schema_name.value().schema_profile == "NOT_AP242");
    REQUIRE(std::any_of(similar_schema_name.value().diagnostics.begin(),
        similar_schema_name.value().diagnostics.end(), [](const std::string& diagnostic) {
            return diagnostic.find("outside the AP242 profile") != std::string::npos;
        }));

    const auto oversized_header = provider.probe(
        std::string("ISO-10303-21; HEADER;") + std::string(64U * 1024U, ' ') +
        "FILE_SCHEMA(('AP242')); ENDSEC;");
    REQUIRE(oversized_header);
    REQUIRE(!oversized_header.value().recognized);

    const auto malformed_header = provider.probe(
        "ISO-10303-21; HEADER; FILE_SCHEMA(AP242); ENDSEC;");
    REQUIRE(malformed_header);
    REQUIRE(!malformed_header.value().recognized);
    REQUIRE(!provider.read_assembly(
        "ISO-10303-21;", "fixture.step", "C",
        digest("settings")));
}

void ui_step_probe_is_bounded_read_only_and_does_not_claim_import() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-engineering-step-probe-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const auto step_path = root / "assembly.step";
    {
        std::ofstream stream(step_path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string header =
            "ISO-10303-21; HEADER; FILE_SCHEMA(('AP242_MANAGED_MODEL_BASED_3D_ENGINEERING')); ENDSEC;\n";
        stream.write(header.data(), static_cast<std::streamsize>(header.size()));
        const std::string tail(
            carto::engineering::kMaxStepHeaderProbeBytes * 16U, 'x');
        stream.write(tail.data(), static_cast<std::streamsize>(tail.size()));
        REQUIRE(stream.good());
    }

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"STEP header probe"}));
    carto::ui::UiController ui(session);
    const auto before = session.snapshot();
    const auto before_ui = ui.snapshot();
    const auto probe = ui.probe_engineering_step(step_path);
    REQUIRE(probe);
    REQUIRE(probe.value().recognized);
    REQUIRE(!probe.value().supported);
    REQUIRE(probe.value().schema_profile == "AP242_MANAGED_MODEL_BASED_3D_ENGINEERING");
    REQUIRE(std::any_of(probe.value().diagnostics.begin(), probe.value().diagnostics.end(),
        [](const std::string& diagnostic) {
            return diagnostic.find("no public CAD kernel") != std::string::npos;
        }));
    REQUIRE(probe.value().units.empty());
    const auto after = session.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.engineering.present == before.engineering.present);
    REQUIRE(ui.snapshot().operations.size() == before_ui.operations.size());

    const auto invalid_path = root / "notes.step";
    {
        std::ofstream stream(invalid_path, std::ios::binary);
        REQUIRE(stream.good());
        stream << "notes: ISO-10303-21; HEADER; FILE_SCHEMA(('AP242')); ENDSEC;";
        REQUIRE(stream.good());
    }
    const auto invalid_probe = ui.probe_engineering_step(invalid_path);
    REQUIRE(invalid_probe);
    REQUIRE(!invalid_probe.value().recognized);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(!ui.probe_engineering_step(root / "missing.step"));
    REQUIRE(session.snapshot().project_revision == before.project_revision);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    try {
        fixture_round_trips_with_identity_and_provenance();
        bom_csv_parser_and_reconciliation_fail_closed_on_ambiguous_evidence();
        requirement_csv_parser_accepts_only_the_declared_dimensional_profile();
        publication_and_vanta_admission_bind_exact_bytes();
        manual_requirement_authoring_is_revision_bound_and_auditable();
        inspection_characteristics_require_current_human_verified_contracts();
        inspection_target_authoring_is_human_revision_and_workcell_bound();
        workcell_edits_are_revisioned_audited_and_invalidate_targets();
        inspection_target_review_is_human_revision_bound_and_preserves_decisions();
        source_invalidation_preserves_authored_records_and_stales_products();
        step_boundary_never_fakes_a_cad_import();
        ui_step_probe_is_bounded_read_only_and_does_not_claim_import();
        application_wires_engineering_authoring_through_persistence_and_history();
        ui_exposes_engineering_workspace_and_immutable_publication();
        ui_imports_bom_csv_through_revision_bound_journal_and_preserves_source_history();
        ui_imports_and_reviews_structured_requirements_without_promoting_proposals();
        ui_attaches_opaque_engineering_evidence_with_safe_provenance();
        ui_imports_exports_and_reconciles_engineering_authoring_files();
        std::cout << "engineering tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
