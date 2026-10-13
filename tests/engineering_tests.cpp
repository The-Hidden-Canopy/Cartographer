#include <carto/engineering/authoring.hpp>
#include <carto/engineering/cad_provider.hpp>
#include <carto/application/application.hpp>
#include <carto/ui/ui.hpp>
#include <carto/project/project.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
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
    REQUIRE(session.snapshot().engineering.vanta_admissible);
    REQUIRE(session.snapshot().engineering.requirement_count == 3U);
    REQUIRE(session.snapshot().engineering.target_count == 4U);
    REQUIRE(session.snapshot().engineering.status.find("admitted") != std::string::npos);

    const auto project_path = root / "dv012.carto";
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{project_path}));

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
    REQUIRE(snapshot.engineering.vanta_admissible);

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
    REQUIRE(ui.snapshot().engineering.vanta_admissible);

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
        publication_and_vanta_admission_bind_exact_bytes();
        source_invalidation_preserves_authored_records_and_stales_products();
        step_boundary_never_fakes_a_cad_import();
        application_wires_engineering_authoring_through_persistence_and_history();
        ui_exposes_engineering_workspace_and_immutable_publication();
        ui_imports_exports_and_reconciles_engineering_authoring_files();
        std::cout << "engineering tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
