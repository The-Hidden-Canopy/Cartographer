#include <carto/application/application.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/ui/ui.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

void fixture_round_trips_and_publishes() {
    const auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    REQUIRE(fixture.value().sources.size() == 1U);
    REQUIRE(fixture.value().assets.size() == 3U);
    REQUIRE(fixture.value().safe_places.size() == 1U);
    REQUIRE(fixture.value().familiarity.size() == 1U);
    REQUIRE(fixture.value().variants.size() == 2U);
    REQUIRE(fixture.value().region_kits.size() == 1U);

    const auto bytes = fixture.value().serialize();
    const auto reopened = carto::condition::AuthoringDocument::deserialize(bytes);
    REQUIRE(reopened);
    REQUIRE(reopened.value().serialize() == bytes);
    REQUIRE(reopened.value().canonical_digest().value() == fixture.value().canonical_digest().value());

    const auto package = carto::condition::Package::publish(
        reopened.value(), carto::core::Revision(4U), reopened.value().project_namespace);
    REQUIRE(package);
    const auto package_reopened = carto::condition::Package::deserialize(package.value().serialize());
    REQUIRE(package_reopened);
    const auto admission = carto::condition::admit_for_consumer(package_reopened.value());
    REQUIRE(admission);
    REQUIRE(admission.value().status == carto::condition::PackageAdmissionStatus::admit);
    REQUIRE(admission.value().admitted_authoring_digest == package.value().authoring_digest);
}

void physical_boundary_rejects_presentation_mutation() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    auto invalid_variant = fixture.value().variants.at(1U);
    invalid_variant.collision_impact_declared = true;
    fixture.value().variants.at(1U) = invalid_variant;
    REQUIRE(!fixture.value().validate());
}

void invalidation_preserves_source_identity_and_blocks_publication() {
    auto fixture = carto::condition::make_condition_fixture();
    if (!fixture) throw TestFailure(fixture.error().message);
    const auto old_asset = fixture.value().assets.front();
    REQUIRE(fixture.value().invalidate_source(1U, "fixture-2", digest("replacement-source")));
    REQUIRE(fixture.value().assets.front().id == old_asset.id);
    REQUIRE(fixture.value().has_stale_sources());
    REQUIRE(!carto::condition::Package::publish(
        fixture.value(), carto::core::Revision(1U), fixture.value().project_namespace));
}

void application_persists_and_undoes_condition_authoring() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-condition-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"CONDITION Source Authoring"}));
    const auto imported = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::ImportConditionFixtureAction{});
    REQUIRE(imported);
    REQUIRE(session.snapshot().condition.present);
    REQUIRE(session.snapshot().condition.publication_ready);
    REQUIRE(session.snapshot().condition.consumer_admissible);
    REQUIRE(session.snapshot().condition.asset_count == 3U);

    const auto project_path = root / "condition.carto";
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened;
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened.snapshot().condition.digest == session.snapshot().condition.digest);

    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened,
        carto::application::InvalidateConditionSourceAction{
            1U, "fixture-2", digest("condition-source-replacement")}));
    REQUIRE(reopened.snapshot().condition.stale);
    REQUIRE(!reopened.snapshot().condition.publication_ready);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::UndoAction{}));
    REQUIRE(reopened.snapshot().condition.publication_ready);
    REQUIRE(carto::application::HumanApplicationAccess::dispatch(
        reopened, carto::application::RedoAction{}));
    REQUIRE(!reopened.snapshot().condition.publication_ready);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void ui_publishes_only_admitted_source_packages() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("cartographer-condition-ui-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"CONDITION UI"}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::condition));
    REQUIRE(ui.import_condition_fixture());
    REQUIRE(ui.snapshot().workspace_kind == carto::ui::Workspace::condition);
    REQUIRE(ui.snapshot().condition.consumer_admissible);
    const auto package = ui.build_condition_package();
    REQUIRE(package);
    const auto path = root / "condition.carto-condition";
    REQUIRE(ui.publish_condition_package(path));
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    REQUIRE(carto::condition::Package::deserialize(bytes));

    REQUIRE(ui.invalidate_condition_source(1U, "fixture-rev-2", digest("condition-rev-2")));
    REQUIRE(ui.snapshot().condition.stale);
    REQUIRE(!ui.snapshot().condition.publication_ready);
    REQUIRE(!ui.build_condition_package());
    REQUIRE(!ui.publish_condition_package(path));
    std::ifstream preserved_stream(path, std::ios::binary);
    REQUIRE(preserved_stream.good());
    const std::string preserved_bytes((std::istreambuf_iterator<char>(preserved_stream)), {});
    REQUIRE(preserved_bytes == bytes);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main() {
    try {
        fixture_round_trips_and_publishes();
        physical_boundary_rejects_presentation_mutation();
        invalidation_preserves_source_identity_and_blocks_publication();
        application_persists_and_undoes_condition_authoring();
        ui_publishes_only_admitted_source_packages();
        return 0;
    } catch (const std::exception& error) {
        return (static_cast<void>(std::fprintf(stderr, "%s\n", error.what())), 1);
    }
}
