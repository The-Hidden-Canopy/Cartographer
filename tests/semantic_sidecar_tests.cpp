#include <carto/assets/blob_store.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include <carto/scientific/model.hpp>
#include <carto/semantic/sidecar.hpp>
#include <carto/world/model.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-semantic-sidecar-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::semantic::SemanticExtensionRecord make_record(
    std::string name_space,
    std::string name,
    std::uint32_t version,
    std::string payload,
    std::vector<carto::world::AuthoringBinding> bindings = {}) {
    carto::semantic::SemanticExtensionRecord record;
    record.schema = carto::semantic::SchemaIdentity{
        std::move(name_space), std::move(name)};
    record.schema_version = version;
    record.payload = std::move(payload);
    record.payload_digest = digest(record.payload);
    record.bound_identities = std::move(bindings);
    return record;
}

carto::semantic::SemanticSidecar fixture_sidecar(bool reverse_order = false) {
    using namespace carto::semantic;
    SemanticSidecar sidecar;
    SemanticExtensionRecord first = make_record(
        "condition.example", "shelter", 3U, "{\"capacity\":4,\"wear\":0.5}",
        {carto::world::AuthoringBinding{
            carto::world::AuthoringBindingKind::scene_object, 7U}});
    SemanticExtensionRecord second = make_record(
        "redmeridian.example", "parcel", 1U, "{\"zone\":\"harbor\"}",
        {carto::world::AuthoringBinding{
            carto::world::AuthoringBindingKind::mesh_asset, 3U}});
    if (reverse_order) {
        require(static_cast<bool>(sidecar.insert_record(std::move(second))),
            "second record enters reversed sidecar");
        require(static_cast<bool>(sidecar.insert_record(std::move(first))),
            "first record enters reversed sidecar");
    } else {
        require(static_cast<bool>(sidecar.insert_record(std::move(first))),
            "first record enters sidecar");
        require(static_cast<bool>(sidecar.insert_record(std::move(second))),
            "second record enters sidecar");
    }
    require(static_cast<bool>(sidecar.insert_manifest(OwnershipManifest{
                carto::world::AuthoringBinding{
                    carto::world::AuthoringBindingKind::scene_object, 7U},
                {{"capacity", FieldOwnership::game_owned},
                 {"roof", FieldOwnership::native_lowerable},
                 {"wear", FieldOwnership::game_owned}},
            })),
        "ownership manifest enters sidecar");
    require(static_cast<bool>(sidecar.refresh_digest()), "sidecar digest refreshes");
    return sidecar;
}

void records_round_trip_deterministically() {
    using namespace carto::semantic;
    const auto first = fixture_sidecar();
    const auto second = fixture_sidecar(true);
    const auto first_digest = first.canonical_digest();
    const auto second_digest = second.canonical_digest();
    require(first_digest && second_digest &&
                first_digest.value() == second_digest.value(),
        "record authoring order does not alter the canonical sidecar digest");
    require(first.serialize() == second.serialize(),
        "record authoring order does not alter the canonical sidecar bytes");

    const std::string encoded = first.serialize();
    const auto reopened = SemanticSidecar::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "sidecar survives deterministic neutral round trip");
    const auto reopened_digest = reopened.value().canonical_digest();
    require(reopened_digest && reopened_digest.value() == first_digest.value(),
        "sidecar identity survives reopen");
    const auto reparsed_record =
        SemanticExtensionRecord::deserialize(first.records().front().serialize());
    require(reparsed_record &&
                reparsed_record.value().serialize() == first.records().front().serialize(),
        "one extension record survives its own round trip");
}

void digest_binds_exact_content() {
    using namespace carto::semantic;
    auto sidecar = fixture_sidecar();
    const auto before = sidecar.canonical_digest();
    require(before && !before.value().is_zero(), "fixture sidecar digest is stable");

    SemanticExtensionRecord drifting = make_record(
        "condition.example", "shelter", 3U, "{\"capacity\":4,\"wear\":0.5}",
        {carto::world::AuthoringBinding{
            carto::world::AuthoringBindingKind::scene_object, 7U}});
    require(static_cast<bool>(drifting.validate()),
        "consistent record validates before drift");
    drifting.payload = "{\"capacity\":5,\"wear\":0.5}";
    require(!drifting.validate(),
        "payload drift fails closed against the payload digest binding");

    require(static_cast<bool>(sidecar.insert_record(make_record(
                "wolves.example", "den", 1U, "{\"occupied\":true}"))),
        "additional record enters sidecar");
    require(!sidecar.validate(),
        "mutating a sidecar without refreshing its digest fails closed");
    const auto stale_digest = sidecar.digest();
    require(static_cast<bool>(sidecar.refresh_digest()),
        "sidecar digest refreshes after mutation");
    require(sidecar.digest() != stale_digest,
        "refreshing rebinds the digest to the exact record set");

    SemanticSidecar duplicates;
    require(static_cast<bool>(duplicates.insert_record(make_record(
                "condition.example", "shelter", 3U, "{\"capacity\":4}"))),
        "record enters duplicate probe sidecar");
    require(!duplicates.insert_record(make_record(
                "condition.example", "shelter", 4U, "{\"capacity\":6}")),
        "duplicate schema identity is rejected");

    SemanticSidecar manifest_duplicate;
    const auto bindings = std::vector<carto::world::AuthoringBinding>{
        carto::world::AuthoringBinding{
            carto::world::AuthoringBindingKind::scene_object, 7U}};
    require(static_cast<bool>(manifest_duplicate.insert_manifest(OwnershipManifest{
                bindings.front(), {{"a", FieldOwnership::game_owned}}})),
        "first manifest enters duplicate probe sidecar");
    require(!manifest_duplicate.insert_manifest(OwnershipManifest{
                bindings.front(), {{"b", FieldOwnership::game_owned}}}),
        "duplicate bound-identity manifest is rejected");
}

void project_current_schema_sidecar_round_trips() {
    using namespace carto;
    TempDirectory temp;
    const auto sidecar = fixture_sidecar();
    const std::string encoded = sidecar.serialize();
    const auto sidecar_digest = sidecar.canonical_digest();
    require(static_cast<bool>(sidecar_digest), "fixture sidecar digest resolves");

    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "sidecar-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "semantic-sidecar-test");
    require(static_cast<bool>(transaction), "sidecar transaction begins");
    require(static_cast<bool>(transaction.value().set_semantic_sidecar(
                encoded, sidecar_digest.value())),
        "transaction admits the digest-bound sidecar bytes");
    require(static_cast<bool>(transaction.value().commit("semantic.sidecar.fixture")),
        "sidecar fixture commits through one journal event");
    require(document.semantic_sidecar_bytes().has_value() &&
                *document.semantic_sidecar_bytes() == encoded,
        "committed document carries the exact sidecar bytes");
    require(document.semantic_sidecar_digest().has_value() &&
                *document.semantic_sidecar_digest() == sidecar_digest.value(),
        "committed document carries the sidecar digest");

    const auto saved = document.save_atomic(temp.path() / "fixture.carto");
    require(static_cast<bool>(saved) &&
                saved.value().schema_version == project::ProjectDocument::kSchemaVersion,
        "document with a semantic sidecar saves atomically");

    const auto loaded = project::ProjectDocument::load(temp.path() / "fixture.carto");
    require(static_cast<bool>(loaded), "project with a semantic sidecar reloads");
    require(loaded.value().schema_version() == project::ProjectDocument::kSchemaVersion,
        "reloaded document reports the current schema version");
    require(loaded.value().semantic_sidecar_bytes().has_value() &&
                *loaded.value().semantic_sidecar_bytes() == encoded,
        "reloaded document preserves the exact sidecar bytes");
    require(loaded.value().semantic_sidecar_digest().has_value() &&
                *loaded.value().semantic_sidecar_digest() == sidecar_digest.value(),
        "reloaded document preserves the sidecar digest");

    const auto reparsed = semantic::SemanticSidecar::deserialize(
        *loaded.value().semantic_sidecar_bytes());
    require(reparsed && reparsed.value().serialize() == encoded,
        "stored bytes reparse through the semantic boundary");

    std::string legacy_v10 = document.serialize();
    const auto mesh_patches = legacy_v10.find("MESH_PATCHES 0\n");
    require(mesh_patches != std::string::npos,
        "schema v10 fixture removes the v29 persistent patch table");
    legacy_v10.erase(mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion) + "\n";
    constexpr std::string_view legacy_header = "CARTOGRAPHER_PROJECT 10\n";
    constexpr std::string_view sculpt_stack_table = "SCULPT_STACKS 0\n";
    constexpr std::string_view garment_asset_table = "GARMENT_ASSETS 0\n";
    constexpr std::string_view cloth_receipt_table = "CLOTH_REST_POSE_RECEIPTS 0\n";
    require(legacy_v10.starts_with(current_header),
        "current project serialization starts with the current schema");
    legacy_v10.replace(0U, current_header.size(), legacy_header);
    const auto stack_record = legacy_v10.find(sculpt_stack_table);
    require(stack_record != std::string::npos,
        "empty current document emits its SculptStack table");
    legacy_v10.erase(stack_record, sculpt_stack_table.size());
    const auto garment_record = legacy_v10.find(garment_asset_table);
    require(garment_record != std::string::npos,
        "empty current document emits its GarmentAsset table");
    legacy_v10.erase(garment_record, garment_asset_table.size());
    const auto cloth_receipt_record = legacy_v10.find(cloth_receipt_table);
    require(cloth_receipt_record != std::string::npos,
        "empty current document emits its cloth rest-pose receipt table");
    legacy_v10.erase(cloth_receipt_record, cloth_receipt_table.size());
    constexpr std::string_view condition_authoring_record = "CONDITION_AUTHORING NONE\n";
    const auto condition_record = legacy_v10.find(condition_authoring_record);
    require(condition_record != std::string::npos,
        "empty current document emits its CONDITION authoring record");
    legacy_v10.erase(condition_record, condition_authoring_record.size());
    constexpr std::string_view presentation_record_prefix = "PRESENTATION_CATALOG ";
    const auto presentation_record = legacy_v10.find(presentation_record_prefix);
    require(presentation_record != std::string::npos,
        "current document emits its presentation catalog record");
    const auto project_end = legacy_v10.rfind("\nEND\n");
    require(project_end != std::string::npos && project_end > presentation_record,
        "project ending follows its presentation catalog record");
    legacy_v10.erase(presentation_record,
        project_end + 1U - presentation_record);
    const auto reopened_v10 = project::ProjectDocument::deserialize(legacy_v10);
    require(static_cast<bool>(reopened_v10) &&
                reopened_v10.value().semantic_sidecar_bytes() ==
                    document.semantic_sidecar_bytes(),
        "v10 project without the newer production tables remains readable");

    const auto empty = project::ProjectDocument();
    const auto empty_serialized = empty.serialize();
    require(empty_serialized.find("SEMANTIC_SIDECAR") == std::string::npos,
        "documents without extensions omit the sidecar record");
    const auto empty_reopened =
        project::ProjectDocument::deserialize(empty_serialized);
    require(static_cast<bool>(empty_reopened) &&
                !empty_reopened.value().semantic_sidecar_bytes().has_value(),
        "v13 document without a sidecar record reloads as unextended");
}

void project_rejects_unbound_sidecar_bytes() {
    using namespace carto;
    TempDirectory temp;
    const auto sidecar = fixture_sidecar();
    const std::string encoded = sidecar.serialize();

    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "reject-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "semantic-sidecar-test");
    require(static_cast<bool>(transaction), "rejection transaction begins");
    require(!transaction.value().set_semantic_sidecar(encoded, digest("other-bytes")),
        "digest that does not bind the bytes is refused");
    require(!transaction.value().set_semantic_sidecar("", digest("empty")),
        "empty sidecar bytes are refused");
    require(!transaction.value().set_semantic_sidecar(
                encoded, assets::Sha256Digest{}),
        "zero sidecar digest is refused");
    require(!document.semantic_sidecar_bytes().has_value(),
        "refused sidecar never reaches the document");
}

void legacy_v9_documents_read_and_upgrade() {
    using namespace carto;
    std::ostringstream legacy;
    legacy << "CARTOGRAPHER_PROJECT 9\n";
    legacy << "AUTHORING \"cartographer.authoring\" \"meters\" \"right_handed_y_up\"\n";
    legacy << "SOURCE_NAMESPACE \"cartographer.project.legacyv9fixture\"\n";
    legacy << "NAME \"legacy-v9\"\n";
    legacy << "REVISION 7\n";
    legacy << "OBJECTS 0\n";
    legacy << "MESHES 0\n";
    legacy << "TOPOLOGY_RECEIPTS 0\n";
    legacy << "SCIENTIFIC_MODEL "
           << std::quoted(scientific::ScientificModel{}.serialize()) << '\n';
    legacy << "WORLD_MODEL " << std::quoted(world::WorldModel{}.serialize()) << '\n';
    legacy << "MATERIAL_CATALOG "
           << std::quoted(project::MaterialCatalog{}.serialize()) << '\n';
    legacy << "END\n";

    const auto upgraded = project::ProjectDocument::deserialize(legacy.str());
    require(static_cast<bool>(upgraded), "v9 document reads cleanly");
    require(upgraded.value().schema_version() == project::ProjectDocument::kSchemaVersion,
        "v9 document upgrades to the current schema version in memory");
    require(upgraded.value().revision().value() == 7U &&
                upgraded.value().name() == "legacy-v9",
        "legacy revision and name survive the upgrade");
    require(!upgraded.value().semantic_sidecar_bytes().has_value(),
        "legacy document carries no semantic sidecar");

    const std::string reencoded = upgraded.value().serialize();
    const std::string current_schema =
        "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion);
    require(reencoded.find(current_schema) == 0U,
        "next save persists the upgraded document at the current schema version");
    const auto rereopened = project::ProjectDocument::deserialize(reencoded);
    require(static_cast<bool>(rereopened) &&
                rereopened.value().serialize() == reencoded,
        "upgraded document round trips deterministically");
}

void malformed_sidecars_and_records_fail_closed() {
    using namespace carto;
    const auto sidecar = fixture_sidecar();
    const std::string encoded = sidecar.serialize();

    require(!semantic::SemanticSidecar::deserialize(encoded + "trailing"),
        "sidecar parser rejects trailing unbound data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_SEMANTIC_SIDECAR 1"}.size(),
        "CARTOGRAPHER_SEMANTIC_SIDECAR 99");
    require(!semantic::SemanticSidecar::deserialize(future),
        "unknown future sidecar versions fail precisely");
    require(!semantic::SemanticSidecar::deserialize(
                "CARTOGRAPHER_SEMANTIC_SIDECAR 1\nRECORDS 65537\n"),
        "hostile record counts are rejected before allocation");
    std::string tampered = encoded;
    const std::string payload_text = "{\\\"capacity\\\":4";
    const auto position = tampered.find(payload_text);
    require(position != std::string::npos, "payload text is present in fixture bytes");
    tampered.replace(position, payload_text.size(), "{\\\"capacity\\\":5");
    require(!semantic::SemanticSidecar::deserialize(tampered),
        "tampered record payload fails closed against its digest binding");

    const auto hostile_record = semantic::SemanticExtensionRecord::deserialize(
        "CARTOGRAPHER_SEMANTIC_EXTENSION_RECORD 42\n");
    require(!hostile_record, "future record versions fail precisely");
}

void project_rejects_malformed_sidecar_record() {
    using namespace carto;
    TempDirectory temp;
    const auto sidecar = fixture_sidecar();
    const std::string encoded = sidecar.serialize();
    const auto sidecar_digest = sidecar.canonical_digest();
    require(static_cast<bool>(sidecar_digest), "fixture sidecar digest resolves");

    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "malformed-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "semantic-sidecar-test");
    require(static_cast<bool>(transaction), "malformed transaction begins");
    require(static_cast<bool>(transaction.value().set_semantic_sidecar(
                encoded, sidecar_digest.value())),
        "transaction admits the fixture sidecar");
    require(static_cast<bool>(transaction.value().commit("semantic.sidecar.fixture")),
        "malformed fixture commits");
    const std::string serialized = document.serialize();
    require(serialized.find("SEMANTIC_SIDECAR ") != std::string::npos,
        "v10 serialization carries the sidecar record");

    const std::string zero_digest = "SEMANTIC_SIDECAR " +
        std::string(64U, '0');
    std::string zeroed = serialized;
    const auto position = zeroed.find("SEMANTIC_SIDECAR ");
    require(position != std::string::npos, "sidecar record is present");
    zeroed.replace(position, zero_digest.size(), zero_digest);
    require(!project::ProjectDocument::deserialize(zeroed),
        "zero sidecar digest in a project file fails closed");

    std::string drifted = serialized;
    const std::string bound_line = "SEMANTIC_SIDECAR " + sidecar_digest.value().hex();
    const auto bound_position = drifted.find(bound_line);
    require(bound_position != std::string::npos, "bound digest line is present");
    drifted.replace(bound_position, bound_line.size(),
        "SEMANTIC_SIDECAR " + digest("drifted").hex());
    require(!project::ProjectDocument::deserialize(drifted),
        "sidecar digest that drifted from its bytes fails closed");
}

} // namespace

int main() {
    records_round_trip_deterministically();
    digest_binds_exact_content();
    project_current_schema_sidecar_round_trips();
    project_rejects_unbound_sidecar_bytes();
    legacy_v9_documents_read_and_upgrade();
    malformed_sidecars_and_records_fail_closed();
    project_rejects_malformed_sidecar_record();
    std::cout << "semantic sidecar tests passed\n";
    return EXIT_SUCCESS;
}
