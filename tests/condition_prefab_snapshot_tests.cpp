#include <carto/condition/authoring.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

void remove_rebase_receipt_block(std::string& encoded) {
    const auto block_start = encoded.find("PREFAB_REBASE_RECEIPTS ");
    const auto block_end = encoded.find("REGION_KITS ", block_start);
    REQUIRE(block_start != std::string::npos && block_end != std::string::npos);
    encoded.erase(block_start, block_end - block_start);
}

carto::condition::AuthoringDocument snapshot_fixture() {
    using namespace carto;
    auto fixture = condition::make_condition_fixture();
    REQUIRE(fixture);
    auto document = std::move(fixture.value());
    document.project_revision = core::Revision{2U};
    condition::PrefabInstanceMember member;
    member.asset_id = document.prefabs.front().asset_ids.front();
    member.source_object_id = 10U;
    member.instance_object_id = 12U;
    member.mesh_asset_id = 5U;
    member.mesh_revision = core::Revision{7U};
    const auto& asset = document.assets.back();
    REQUIRE(asset.id == member.asset_id);
    member.source_revision = asset.source_revision;
    member.source_digest = asset.source_digest;
    member.source_world_transform.translation = {10.0, 20.0, 30.0};
    member.instance_mesh_asset_id = 6U;
    // This codec fixture supplies opaque evidence; geometry verification is
    // performed at the project/application boundary, not by this data model.
    member.snapshot_mesh_digest = member.source_digest;
    member.source_snapshot_mesh_digest = member.source_digest;
    member.instance_mesh_revision = member.mesh_revision;
    member.instance_local_transform.translation = {1.25, -2.0, 3.5};
    member.instance_local_transform.scale = {1.0, 2.0, 1.0};
    condition::PrefabInstanceRecord instance;
    instance.id = 1U;
    instance.prefab_id = document.prefabs.front().id;
    instance.name = "Isolated codec fixture";
    instance.root_object_id = 11U;
    instance.captured_project_revision = core::Revision{9U};
    instance.source_authoring_revision = core::Revision{1U};
    instance.prefab_source_revision = document.prefabs.front().source_revision;
    instance.members.push_back(std::move(member));
    document.prefab_instances.push_back(std::move(instance));
    REQUIRE(document.validate());
    return document;
}

std::string legacy_v9_bytes(const carto::condition::AuthoringDocument& document) {
    auto encoded = document.serialize();
    const auto header_end = encoded.find('\n');
    REQUIRE(header_end != std::string::npos);
    encoded.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 9");
    const auto member_start = encoded.find("PREFAB_INSTANCE_MEMBER ");
    const auto member_end = encoded.find('\n', member_start);
    REQUIRE(member_start != std::string::npos && member_end != std::string::npos);
    const auto& member = document.prefab_instances.front().members.front();
    const auto& source = member.source_world_transform;
    std::ostringstream old_member;
    old_member.imbue(std::locale::classic());
    old_member << std::setprecision(17) << "PREFAB_INSTANCE_MEMBER " << member.asset_id << ' '
        << member.source_object_id << ' ' << member.instance_object_id << ' '
        << member.mesh_asset_id << ' ' << member.mesh_revision.value() << ' '
        << std::quoted(member.source_revision) << ' ' << member.source_digest.hex() << ' '
        << source.translation.x << ' ' << source.translation.y << ' ' << source.translation.z << ' '
        << source.rotation.x << ' ' << source.rotation.y << ' ' << source.rotation.z << ' '
        << source.rotation.w << ' ' << source.scale.x << ' ' << source.scale.y << ' ' << source.scale.z;
    encoded.replace(member_start, member_end - member_start, old_member.str());
    remove_rebase_receipt_block(encoded);
    return encoded;
}

std::string legacy_v10_bytes(const carto::condition::AuthoringDocument& document) {
    auto encoded = document.serialize();
    const auto header_end = encoded.find('\n');
    REQUIRE(header_end != std::string::npos);
    encoded.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 10");
    const auto member_start = encoded.find("PREFAB_INSTANCE_MEMBER ");
    const auto member_end = encoded.find('\n', member_start);
    REQUIRE(member_start != std::string::npos && member_end != std::string::npos);
    const auto& member = document.prefab_instances.front().members.front();
    const auto write_transform = [](std::ostream& output, const carto::core::Transform& value) {
        output << value.translation.x << ' ' << value.translation.y << ' ' << value.translation.z << ' '
            << value.rotation.x << ' ' << value.rotation.y << ' ' << value.rotation.z << ' '
            << value.rotation.w << ' ' << value.scale.x << ' ' << value.scale.y << ' ' << value.scale.z;
    };
    std::ostringstream old_member;
    old_member.imbue(std::locale::classic());
    old_member << std::setprecision(17) << "PREFAB_INSTANCE_MEMBER " << member.asset_id << ' '
        << member.source_object_id << ' ' << member.instance_object_id << ' '
        << member.mesh_asset_id << ' ' << member.mesh_revision.value() << ' '
        << std::quoted(member.source_revision) << ' ' << member.source_digest.hex() << ' ';
    write_transform(old_member, member.source_world_transform);
    old_member << ' ' << member.instance_mesh_asset_id << ' '
        << member.snapshot_mesh_digest.hex() << ' ';
    write_transform(old_member, member.instance_local_transform);
    encoded.replace(member_start, member_end - member_start, old_member.str());
    remove_rebase_receipt_block(encoded);
    return encoded;
}

std::string legacy_v11_bytes(const carto::condition::AuthoringDocument& document) {
    auto encoded = document.serialize();
    const auto header_end = encoded.find('\n');
    REQUIRE(header_end != std::string::npos);
    encoded.replace(0U, header_end, "CARTOGRAPHER_CONDITION_AUTHORING 11");
    remove_rebase_receipt_block(encoded);
    return encoded;
}

void isolated_snapshot_contract_round_trips_exactly() {
    using namespace carto;
    const auto document = snapshot_fixture();
    const auto encoded = document.serialize();
    REQUIRE(encoded.starts_with("CARTOGRAPHER_CONDITION_AUTHORING 12\n"));
    const auto reopened = condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened);
    REQUIRE(reopened.value().serialize() == encoded);
    REQUIRE(document.canonical_digest());
    REQUIRE(reopened.value().canonical_digest().value() == document.canonical_digest().value());
    const auto& member = reopened.value().prefab_instances.front().members.front();
    REQUIRE(member.mesh_asset_id == 5U);
    REQUIRE(member.instance_mesh_asset_id == 6U);
    REQUIRE(member.mesh_revision == core::Revision{7U});
    REQUIRE(!member.snapshot_mesh_digest.is_zero());
    REQUIRE(member.source_snapshot_mesh_digest == member.snapshot_mesh_digest);
    REQUIRE(member.instance_mesh_revision == member.mesh_revision);
    REQUIRE(member.instance_local_transform.translation.x == 1.25);
    REQUIRE(member.instance_local_transform.translation.y == -2.0);
    REQUIRE(member.instance_local_transform.translation.z == 3.5);
    REQUIRE(member.instance_local_transform.scale.y == 2.0);
    REQUIRE(member.source_world_transform.translation.x == 10.0);
    REQUIRE(member.source_world_transform.translation.y == 20.0);
    REQUIRE(document.validate_for_publication());
    REQUIRE(condition::Package::publish(document, core::Revision{10U}, document.project_namespace));
}

void legacy_v9_keeps_source_receipts_without_inventing_snapshot_evidence() {
    using namespace carto;
    const auto source = snapshot_fixture();
    const auto migrated = condition::AuthoringDocument::deserialize(legacy_v9_bytes(source));
    REQUIRE(migrated);
    const auto& member = migrated.value().prefab_instances.front().members.front();
    REQUIRE(member.mesh_asset_id == 5U && member.mesh_revision == core::Revision{7U});
    REQUIRE(member.source_object_id == 10U && member.instance_object_id == 12U);
    REQUIRE(member.source_digest == source.prefab_instances.front().members.front().source_digest);
    REQUIRE(member.instance_mesh_asset_id == 0U);
    REQUIRE(member.snapshot_mesh_digest.is_zero());
    REQUIRE(member.source_snapshot_mesh_digest.is_zero());
    REQUIRE(member.instance_mesh_revision.value() == 0U);
    REQUIRE(member.instance_local_transform.translation.x == 0.0);
    REQUIRE(member.instance_local_transform.rotation.w == 1.0);
    REQUIRE(member.instance_local_transform.scale.y == 1.0);
    const auto rewritten = migrated.value().serialize();
    REQUIRE(rewritten.starts_with("CARTOGRAPHER_CONDITION_AUTHORING 12\n"));
    const auto reopened = condition::AuthoringDocument::deserialize(rewritten);
    REQUIRE(reopened && reopened.value().serialize() == rewritten);
    REQUIRE(reopened.value().prefab_instances.front().members.front().instance_mesh_asset_id == 0U);
    REQUIRE(!migrated.value().validate_for_publication());
    REQUIRE(!condition::Package::publish(
        migrated.value(), core::Revision{10U}, migrated.value().project_namespace));
    // Known legacy state also fails the consumer gate when encountered inside
    // a correctly checksummed older or rewritten package envelope.
    for (const auto& bytes : {legacy_v9_bytes(source), rewritten}) {
        condition::Package package;
        package.project_namespace = migrated.value().project_namespace;
        package.project_revision = core::Revision{10U};
        package.authoring_bytes = bytes;
        package.authoring_digest = assets::sha256(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        REQUIRE(package.refresh_digest());
        const auto admitted = condition::admit_for_consumer(package);
        REQUIRE(admitted && admitted.value().status == condition::PackageAdmissionStatus::refuse);
        REQUIRE(!admitted.value().reasons.empty());
    }
}

void legacy_v10_preserves_distinct_snapshot_receipts() {
    using namespace carto;
    const auto source = snapshot_fixture();
    const auto& member = source.prefab_instances.front().members.front();
    const auto migrated = condition::AuthoringDocument::deserialize(legacy_v10_bytes(source));
    REQUIRE(migrated);
    const auto& converted = migrated.value().prefab_instances.front().members.front();
    REQUIRE(converted.source_snapshot_mesh_digest == member.snapshot_mesh_digest);
    REQUIRE(converted.snapshot_mesh_digest == member.snapshot_mesh_digest);
    REQUIRE(converted.instance_mesh_revision == member.mesh_revision);
    REQUIRE(migrated.value().serialize().starts_with(
        "CARTOGRAPHER_CONDITION_AUTHORING 12\n"));
}

void legacy_v11_preserves_split_mesh_receipts_without_rebase_history() {
    using namespace carto;
    const auto source = snapshot_fixture();
    const auto migrated = condition::AuthoringDocument::deserialize(legacy_v11_bytes(source));
    REQUIRE(migrated);
    const auto& source_member = source.prefab_instances.front().members.front();
    const auto& converted = migrated.value().prefab_instances.front().members.front();
    REQUIRE(converted.source_snapshot_mesh_digest == source_member.source_snapshot_mesh_digest);
    REQUIRE(converted.snapshot_mesh_digest == source_member.snapshot_mesh_digest);
    REQUIRE(converted.instance_mesh_revision == source_member.instance_mesh_revision);
    REQUIRE(migrated.value().prefab_rebase_receipts.empty());
    REQUIRE(migrated.value().serialize().starts_with(
        "CARTOGRAPHER_CONDITION_AUTHORING 12\n"));
}

void prefab_rebase_receipt_round_trips_and_rejects_inconsistent_choices() {
    using namespace carto;
    auto document = snapshot_fixture();
    condition::PrefabRebaseReceipt receipt;
    receipt.instance_id = document.prefab_instances.front().id;
    receipt.reviewed_authoring_revision = core::Revision{2U};
    const auto reviewed_digest = document.canonical_digest();
    REQUIRE(reviewed_digest);
    receipt.reviewed_authoring_digest = reviewed_digest.value();
    receipt.applied_authoring_revision = core::Revision{3U};
    document.project_revision = core::Revision{3U};
    document.prefab_instances.front().source_authoring_revision = core::Revision{3U};
    receipt.justification = "Keep reviewed geometry and placement.";
    receipt.decisions.push_back(condition::PrefabRebaseMemberDecision{
        document.prefab_instances.front().members.front().asset_id,
        condition::PrefabRebaseMeshDecision::keep_instance_snapshot,
        condition::PrefabRebasePlacementDecision::keep_instance_placement,
        condition::PrefabRebaseMembershipDecision::retained});
    document.prefab_rebase_receipts.push_back(receipt);
    REQUIRE(document.validate());
    const auto encoded = document.serialize();
    const auto reopened = condition::AuthoringDocument::deserialize(encoded);
    REQUIRE(reopened && reopened.value().serialize() == encoded);
    REQUIRE(reopened.value().prefab_rebase_receipts.size() == 1U);
    const auto& restored = reopened.value().prefab_rebase_receipts.front();
    REQUIRE(restored.reviewed_authoring_digest == receipt.reviewed_authoring_digest);
    REQUIRE(restored.justification == receipt.justification);
    REQUIRE(restored.decisions.front().mesh ==
        condition::PrefabRebaseMeshDecision::keep_instance_snapshot);

    document.prefab_rebase_receipts.front().justification = "   ";
    REQUIRE(!document.validate());
    document.prefab_rebase_receipts.front() = receipt;
    document.prefab_rebase_receipts.front().decisions.front().membership =
        condition::PrefabRebaseMembershipDecision::added;
    REQUIRE(!document.validate());
}

void partial_aliased_and_non_finite_snapshot_records_fail_closed() {
    using namespace carto;
    for (std::size_t mutation = 0U; mutation < 10U; ++mutation) {
        auto document = snapshot_fixture();
        auto& member = document.prefab_instances.front().members.front();
        switch (mutation) {
        case 0U: member.snapshot_mesh_digest = {}; break;
        case 1U: member.instance_mesh_asset_id = 0U; break;
        case 2U: member.instance_mesh_asset_id = member.mesh_asset_id; break;
        case 3U:
            member.instance_mesh_asset_id = 0U;
            member.snapshot_mesh_digest = {};
            break; // A legacy marker cannot retain an invented local baseline.
        case 4U:
            member.instance_local_transform.translation.x = std::numeric_limits<double>::infinity();
            break;
        case 5U:
            member.instance_local_transform.scale.z = std::numeric_limits<double>::quiet_NaN();
            break;
        case 6U: member.instance_local_transform.rotation = {0.0, 0.0, 0.0, 0.0}; break;
        case 7U:
            member.instance_mesh_asset_id = 0U;
            member.snapshot_mesh_digest = {};
            member.source_snapshot_mesh_digest = {};
            member.instance_mesh_revision = {};
            member.instance_local_transform = core::Transform::identity();
            member.instance_local_transform.rotation.w = -1.0;
            break;
        case 8U: member.source_snapshot_mesh_digest = {}; break;
        case 9U: member.instance_mesh_revision = {}; break;
        default: REQUIRE(false); break;
        }
        REQUIRE(!document.validate());
        REQUIRE(!condition::AuthoringDocument::deserialize(document.serialize()));
    }
    auto truncated = snapshot_fixture().serialize();
    const auto line_start = truncated.find("PREFAB_INSTANCE_MEMBER ");
    const auto line_end = truncated.find('\n', line_start);
    const auto final_field = truncated.rfind(' ', line_end);
    REQUIRE(final_field != std::string::npos);
    truncated.erase(final_field, line_end - final_field);
    REQUIRE(!condition::AuthoringDocument::deserialize(truncated));
    auto wrong_version = snapshot_fixture().serialize();
    wrong_version.replace(0U, wrong_version.find('\n'), "CARTOGRAPHER_CONDITION_AUTHORING 9");
    REQUIRE(!condition::AuthoringDocument::deserialize(wrong_version));
}

void isolated_members_cannot_alias_other_snapshots() {
    using namespace carto;
    auto document = snapshot_fixture();
    auto second = document.prefab_instances.front();
    second.id = 2U;
    second.root_object_id = 21U;
    second.members.front().instance_object_id = 22U;
    document.prefab_instances.push_back(std::move(second));
    REQUIRE(!document.validate());
    REQUIRE(!condition::AuthoringDocument::deserialize(document.serialize()));
}

void isolated_child_can_be_an_explicit_prototype_for_a_fresh_snapshot() {
    using namespace carto;
    auto document = snapshot_fixture();
    auto prototype_asset = document.assets.back();
    prototype_asset.id = 4U;
    prototype_asset.stable_name = "Explicit instance child prototype";
    prototype_asset.authored_object_id = 12U;
    document.assets.push_back(std::move(prototype_asset));
    auto prototype_prefab = document.prefabs.front();
    prototype_prefab.id = 2U;
    prototype_prefab.name = "Reused instance prototype";
    prototype_prefab.asset_ids = {4U};
    document.prefabs.push_back(std::move(prototype_prefab));
    auto second = document.prefab_instances.front();
    second.id = 2U;
    second.prefab_id = 2U;
    second.root_object_id = 21U;
    auto& member = second.members.front();
    member.asset_id = 4U;
    member.source_object_id = 12U;
    member.instance_object_id = 22U;
    member.mesh_asset_id = 6U; // The first instance's independently captured mesh.
    member.instance_mesh_asset_id = 7U; // This placement owns a fresh copy.
    document.prefab_instances.push_back(std::move(second));
    REQUIRE(document.validate());
    const auto reopened = condition::AuthoringDocument::deserialize(document.serialize());
    REQUIRE(reopened && reopened.value().serialize() == document.serialize());
    REQUIRE(reopened.value().prefab_instances[1].members.front().mesh_asset_id ==
        reopened.value().prefab_instances[0].members.front().instance_mesh_asset_id);
}

class CommaPunctuation final : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

void snapshot_codec_is_independent_of_process_locale() {
    using namespace carto;
    const auto document = snapshot_fixture();
    const auto canonical = document.serialize();
    struct LocaleRestore {
        std::locale previous = std::locale();
        ~LocaleRestore() { std::locale::global(previous); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new CommaPunctuation));
    REQUIRE(document.serialize() == canonical);
    const auto decoded = condition::AuthoringDocument::deserialize(canonical);
    REQUIRE(decoded && decoded.value().serialize() == canonical);
}

} // namespace

int main() {
    try {
        isolated_snapshot_contract_round_trips_exactly();
        legacy_v9_keeps_source_receipts_without_inventing_snapshot_evidence();
        legacy_v10_preserves_distinct_snapshot_receipts();
        legacy_v11_preserves_split_mesh_receipts_without_rebase_history();
        prefab_rebase_receipt_round_trips_and_rejects_inconsistent_choices();
        partial_aliased_and_non_finite_snapshot_records_fail_closed();
        isolated_members_cannot_alias_other_snapshots();
        isolated_child_can_be_an_explicit_prototype_for_a_fresh_snapshot();
        snapshot_codec_is_independent_of_process_locale();
        std::cout << "condition prefab snapshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
