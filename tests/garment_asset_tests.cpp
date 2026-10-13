#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/garment_asset.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-garment-tests-" + std::to_string(stamp) + "-" +
                std::to_string(sequence.fetch_add(1U)));
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

carto::geometry::EditableMesh make_two_panel_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {2.0, 0.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{6U}, {2.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{5U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{2U}, geometry::VertexId{3U},
             geometry::VertexId{6U}, geometry::VertexId{5U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::project::ProjectDocument make_project(
    carto::journal::Journal& journal,
    carto::geometry::EditableMesh mesh,
    std::uint64_t& mesh_asset) {
    using namespace carto;
    auto document = project::ProjectDocument::create("GarmentAsset Test");
    REQUIRE(document);
    const std::string snapshot = document.value().serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(bytes, snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "garment-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("mesh.fixture"));
    return std::move(document).value();
}

carto::production::GarmentAsset make_authored_asset(
    const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset) {
    using namespace carto;
    auto created = production::make_garment_asset(document, mesh_asset, "jacket.work");
    REQUIRE(created);
    production::GarmentAsset asset = std::move(created).value();
    asset.panels = {
        {"panel.left", {geometry::FaceId{1U}}},
        {"panel.right", {geometry::FaceId{2U}}}};
    asset.seams.push_back(production::GarmentSeam{
        "seam.side", {geometry::VertexId{1U}, geometry::VertexId{4U}},
        {geometry::VertexId{3U}, geometry::VertexId{6U}}, 0.002});
    asset.collision_capsules.push_back(production::GarmentCollisionCapsule{
        "body.torso", {-1.0, -0.25, 0.0}, {3.0, -0.25, 0.0}, 0.2});
    asset.pins.push_back(production::GarmentPin{
        "pin.shoulder", geometry::VertexId{1U}, {0.0, 0.0, 0.0}, 0.0});
    REQUIRE(asset.validate(document.meshes().at(mesh_asset)));
    return asset;
}

void source_round_trip_and_hostile_input_are_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "garment-source-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_two_panel_mesh(), mesh_asset);
    const production::GarmentAsset asset = make_authored_asset(document, mesh_asset);
    const std::string bytes = asset.serialize();
    REQUIRE(!bytes.empty());
    const auto decoded = production::GarmentAsset::deserialize(bytes);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == bytes);
    REQUIRE(decoded.value().validate(document.meshes().at(mesh_asset)));
    REQUIRE(decoded.value().digest().value() == asset.digest().value());

    std::string tampered = bytes;
    const std::size_t fabric = tampered.find("FABRIC ");
    REQUIRE(fabric != std::string::npos);
    tampered.replace(fabric + 7U, 16U, std::string(16U, '0'));
    REQUIRE(!production::GarmentAsset::deserialize(tampered));

    auto duplicate = asset;
    duplicate.panels[1].faces.push_back(geometry::FaceId{1U});
    std::sort(duplicate.panels[1].faces.begin(), duplicate.panels[1].faces.end());
    REQUIRE(!duplicate.validate(document.meshes().at(mesh_asset)));
    auto bad_seam = asset;
    bad_seam.seams.front().first_chain = {geometry::VertexId{1U}, geometry::VertexId{5U}};
    REQUIRE(!bad_seam.validate(document.meshes().at(mesh_asset)));
    auto bad_pin = asset;
    bad_pin.pins.front().target_position.x = std::numeric_limits<double>::infinity();
    REQUIRE(!bad_pin.validate());
    auto bad_collision = asset;
    bad_collision.collision_capsules.front().radius = 0.0;
    REQUIRE(!bad_collision.validate());

    auto stale_mesh = document.meshes().at(mesh_asset);
    REQUIRE(stale_mesh.set_vertex_position(geometry::VertexId{1U}, {0.0, 0.0, 0.25}));
    REQUIRE(!asset.validate(stale_mesh));
}

void attachment_v2_round_trip_and_legacy_v1_compatibility_are_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "garment-attachment-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_two_panel_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    production::GarmentAsset asset = make_authored_asset(document, mesh_asset);
    asset.attachments.push_back(production::GarmentAttachment{
        "attachment.shoulder", "body.shoulder", geometry::VertexId{2U},
        {1.0, 0.25, 0.0}, 0.0});
    REQUIRE(asset.validate(source));

    const std::string v2_bytes = asset.serialize();
    REQUIRE(v2_bytes.starts_with("CARTO_GARMENT_ASSET 2\n"));
    REQUIRE(v2_bytes.find("ATTACHMENTS 1\n") != std::string::npos);
    const auto decoded = production::GarmentAsset::deserialize(v2_bytes);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == v2_bytes);
    REQUIRE(decoded.value().attachments.size() == 1U);
    REQUIRE(decoded.value().attachments.front().identity == "attachment.shoulder");
    REQUIRE(decoded.value().attachments.front().target_identity == "body.shoulder");
    REQUIRE(decoded.value().attachments.front().vertex == geometry::VertexId{2U});
    REQUIRE(decoded.value().digest().value() == asset.digest().value());

    std::string invalid_target = v2_bytes;
    const std::size_t attachment_record = invalid_target.find("ATTACHMENT ");
    REQUIRE(attachment_record != std::string::npos);
    const std::size_t target_identity = invalid_target.find(' ', attachment_record + 11U);
    REQUIRE(target_identity != std::string::npos);
    const std::size_t target_identity_end = invalid_target.find(' ', target_identity + 1U);
    REQUIRE(target_identity_end != std::string::npos);
    invalid_target.replace(target_identity + 1U,
        target_identity_end - target_identity - 1U, "00");
    REQUIRE(!production::GarmentAsset::deserialize(invalid_target));

    auto empty_target = asset;
    empty_target.attachments.front().target_identity.clear();
    REQUIRE(!empty_target.validate());
    auto nonfinite_target = asset;
    nonfinite_target.attachments.front().target_position.z =
        std::numeric_limits<double>::infinity();
    REQUIRE(!nonfinite_target.validate());
    auto duplicate_identity = asset;
    duplicate_identity.attachments.front().identity = "pin.shoulder";
    REQUIRE(!duplicate_identity.validate());
    auto duplicate_vertex = asset;
    duplicate_vertex.attachments.front().vertex = geometry::VertexId{1U};
    REQUIRE(!duplicate_vertex.validate());
    auto missing_vertex = asset;
    missing_vertex.attachments.front().vertex = geometry::VertexId{999U};
    REQUIRE(!missing_vertex.validate(source));
    auto unsupported_v1 = asset;
    unsupported_v1.format_version = 1U;
    REQUIRE(!unsupported_v1.validate());

    auto legacy = asset;
    legacy.attachments.clear();
    legacy.format_version = 1U;
    const std::string v1_bytes = legacy.serialize();
    REQUIRE(v1_bytes.starts_with("CARTO_GARMENT_ASSET 1\n"));
    REQUIRE(v1_bytes.find("ATTACHMENTS ") == std::string::npos);
    const auto restored_v1 = production::GarmentAsset::deserialize(v1_bytes);
    REQUIRE(restored_v1);
    REQUIRE(restored_v1.value().format_version == 1U);
    REQUIRE(restored_v1.value().serialize() == v1_bytes);
}

void project_transaction_round_trip_and_compare_swap_are_preserved() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "garment-transaction-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_two_panel_mesh(), mesh_asset);
    production::GarmentAsset asset = make_authored_asset(document, mesh_asset);
    asset.attachments.push_back(production::GarmentAttachment{
        "attachment.persistence", "body.root", geometry::VertexId{2U},
        {1.0, 0.0, 0.0}, 0.0});
    const auto digest = asset.digest();
    REQUIRE(digest);

    auto first = project::ProjectTransaction::begin(document, journal, "garment-tests");
    REQUIRE(first);
    REQUIRE(production::stage_garment_asset(first.value(), asset));
    REQUIRE(document.garment_assets().empty());
    REQUIRE(first.value().commit(production::garment_asset_transaction_operation(
        digest.value())));
    REQUIRE(document.garment_assets().contains(mesh_asset));
    const auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().schema_version() == project::ProjectDocument::kSchemaVersion);
    REQUIRE(reopened.value().garment_assets().at(mesh_asset).bytes == asset.serialize());
    REQUIRE(reopened.value().garment_assets().at(mesh_asset).digest == digest.value());
    const auto decoded = production::GarmentAsset::deserialize(
        reopened.value().garment_assets().at(mesh_asset).bytes);
    REQUIRE(decoded && decoded.value().validate(reopened.value().meshes().at(mesh_asset)));

    std::string corrupt_project = document.serialize();
    const std::size_t garment_record = corrupt_project.find("GARMENT_ASSET ");
    REQUIRE(garment_record != std::string::npos);
    const std::size_t mesh_end = corrupt_project.find(' ', garment_record + 14U);
    REQUIRE(mesh_end != std::string::npos);
    const std::size_t digest_start = mesh_end + 1U;
    corrupt_project[digest_start] = corrupt_project[digest_start] == '0' ? '1' : '0';
    REQUIRE(!project::ProjectDocument::deserialize(corrupt_project));

    std::string legacy_v11 = document.serialize();
    const std::size_t garment_section = legacy_v11.find("GARMENT_ASSETS ");
    REQUIRE(garment_section != std::string::npos);
    const std::size_t asset_sentinel = legacy_v11.find(
        "END_GARMENT_ASSET\n\"", garment_section);
    REQUIRE(asset_sentinel != std::string::npos);
    const std::size_t serialized_asset_end = asset_sentinel +
        std::string("END_GARMENT_ASSET\n\"").size();
    const std::size_t asset_end = legacy_v11.find('\n', serialized_asset_end);
    REQUIRE(asset_end != std::string::npos);
    legacy_v11.erase(garment_section, asset_end + 1U - garment_section);
    constexpr std::string_view cloth_receipt_table = "CLOTH_REST_POSE_RECEIPTS 0\n";
    const std::size_t cloth_receipts = legacy_v11.find(cloth_receipt_table);
    REQUIRE(cloth_receipts != std::string::npos);
    legacy_v11.erase(cloth_receipts, cloth_receipt_table.size());
    constexpr std::string_view condition_authoring_record = "CONDITION_AUTHORING NONE\n";
    const std::size_t condition_record = legacy_v11.find(condition_authoring_record);
    REQUIRE(condition_record != std::string::npos);
    legacy_v11.erase(condition_record, condition_authoring_record.size());
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion);
    const std::size_t header = legacy_v11.find(current_header);
    REQUIRE(header != std::string::npos);
    legacy_v11.replace(header, current_header.size(), "CARTOGRAPHER_PROJECT 11");
    const auto legacy_reopened = project::ProjectDocument::deserialize(legacy_v11);
    if (!legacy_reopened) throw std::runtime_error(legacy_reopened.error().message);
    REQUIRE(legacy_reopened.value().garment_assets().empty());

    asset.fabric.damping = 0.2;
    auto update = project::ProjectTransaction::begin(document, journal, "garment-tests");
    REQUIRE(update);
    REQUIRE(!production::stage_garment_asset(update.value(), asset));
    REQUIRE(production::stage_garment_asset(update.value(), asset, digest.value()));
    const auto next_digest = asset.digest();
    REQUIRE(next_digest);
    REQUIRE(update.value().commit(production::garment_asset_transaction_operation(
        next_digest.value())));
    REQUIRE(document.garment_assets().at(mesh_asset).digest == next_digest.value());

    auto wrong_mesh = project::ProjectTransaction::begin(document, journal, "garment-tests");
    REQUIRE(wrong_mesh);
    auto foreign = asset;
    foreign.project_namespace = "another.project";
    REQUIRE(!production::stage_garment_asset(
        wrong_mesh.value(), foreign, next_digest.value()));
    REQUIRE(wrong_mesh.value().rollback());
}

} // namespace

int main() {
    try {
        source_round_trip_and_hostile_input_are_closed();
        std::cout << "PASS GarmentAsset canonical source round trip, boundary validation and stale-mesh rejection\n";
        attachment_v2_round_trip_and_legacy_v1_compatibility_are_closed();
        std::cout << "PASS versioned GarmentAsset attachment round trip, hostile anchors and v1 compatibility\n";
        project_transaction_round_trip_and_compare_swap_are_preserved();
        std::cout << "PASS journaled GarmentAsset persistence, schema reopen and compare-and-swap update\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL GarmentAsset tests: " << error.what() << '\n';
        return 1;
    }
}
