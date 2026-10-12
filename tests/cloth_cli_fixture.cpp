#include <carto/assets/blob_store.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/cloth_solver.hpp>
#include <carto/production/garment_asset.hpp>
#include <carto/production/closure.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

carto::geometry::EditableMesh make_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    auto inserted = mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}, geometry::VertexId{4U}}}});
    require(static_cast<bool>(inserted), "could not create the cloth CLI fixture mesh");
    attributes::AttributeSet attribute_set({{
        attributes::AttributeDomain::corner, 4U}});
    require(static_cast<bool>(attribute_set.add_layer(attributes::AttributeLayer{
            {"uv0.corners", attributes::AttributeDomain::corner,
             attributes::AttributeType::vec2},
            {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
             core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}})),
        "could not create the cloth CLI fixture UV layer");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
            1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "could not create the cloth CLI fixture UV set");
    require(static_cast<bool>(mesh.set_attribute_payload(
            std::move(attribute_set), std::move(uv_sets))),
        "could not attach the cloth CLI fixture UV payload");
    return mesh;
}

int create_fixture(const std::filesystem::path& path) {
    using namespace carto;
    auto created = project::ProjectDocument::create("Cloth CLI Fixture");
    require(static_cast<bool>(created), "could not create the fixture project");
    project::ProjectDocument document = std::move(created).value();
    journal::Journal journal(path.string() + ".fixture-journal.log");
    const std::string initial_snapshot = document.serialize();
    const auto* initial_bytes = reinterpret_cast<const std::uint8_t*>(initial_snapshot.data());
    require(static_cast<bool>(journal.append(journal::JournalAppend{
                core::Revision{}, document.revision(), "project.create",
                std::span<const std::uint8_t>(initial_bytes, initial_snapshot.size()),
                std::nullopt})),
        "could not anchor the fixture project revision in its journal");
    auto mesh_transaction = project::ProjectTransaction::begin(
        document, journal, "cloth-cli-fixture");
    if (!mesh_transaction) {
        throw std::runtime_error("could not begin the mesh fixture transaction: " +
            mesh_transaction.error().message);
    }
    auto mesh_asset = mesh_transaction.value().add_mesh(make_mesh());
    require(static_cast<bool>(mesh_asset), "could not add the fixture mesh");
    require(static_cast<bool>(mesh_transaction.value().commit("fixture.mesh.create")),
        "could not commit the fixture mesh");

    auto garment = production::make_garment_asset(
        document, mesh_asset.value(), "cloth-cli.fixture-garment");
    require(static_cast<bool>(garment), "could not author the fixture GarmentAsset");
    for (const geometry::Vertex& vertex :
         document.meshes().at(mesh_asset.value()).vertices_sorted()) {
        garment.value().pins.push_back(production::GarmentPin{
            "pin." + std::to_string(vertex.id.value), vertex.id, vertex.position, 0.0});
    }
    auto garment_transaction = project::ProjectTransaction::begin(
        document, journal, "cloth-cli-fixture");
    require(static_cast<bool>(garment_transaction), "could not begin the garment fixture transaction");
    require(static_cast<bool>(production::stage_garment_asset(
                garment_transaction.value(), garment.value())),
        "could not stage the fixture GarmentAsset");
    require(static_cast<bool>(garment_transaction.value().commit("fixture.garment.create")),
        "could not commit the fixture GarmentAsset");
    require(static_cast<bool>(document.save_atomic(path)), "could not save the fixture project");
    return 0;
}

int mutate_fixture(const std::filesystem::path& path) {
    using namespace carto;
    auto loaded = project::ProjectDocument::load(path);
    require(static_cast<bool>(loaded), "could not reopen the AO CLI fixture project");
    project::ProjectDocument document = std::move(loaded).value();
    const auto source = document.meshes().find(1U);
    require(source != document.meshes().end(), "AO CLI fixture mesh is missing");
    geometry::EditableMesh changed = source->second;
    require(static_cast<bool>(changed.set_vertex_position(
            geometry::VertexId{1U}, {0.0, 0.0, 0.25})),
        "could not alter the AO CLI fixture mesh");
    journal::Journal journal(path.string() + ".fixture-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "bake-cli-fixture");
    require(static_cast<bool>(transaction), "could not begin the AO fixture mutation");
    require(static_cast<bool>(transaction.value().replace_mesh(1U, std::move(changed))),
        "could not stage the AO fixture mesh mutation");
    require(static_cast<bool>(transaction.value().commit("fixture.mesh.mutate")),
        "could not commit the AO fixture mesh mutation");
    require(static_cast<bool>(document.save_atomic(path)),
        "could not save the mutated AO fixture project");
    return 0;
}

int verify_output(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_root) {
    using namespace carto;
    auto source = project::ProjectDocument::load(source_path);
    auto output = project::ProjectDocument::load(output_root / "project.carto");
    require(static_cast<bool>(source), "input project no longer loads");
    require(static_cast<bool>(output), "published project does not load");
    require(source.value().cloth_rest_pose_receipts().empty(),
        "cloth CLI mutated the input project");
    require(source.value().meshes().size() == 1U, "input project mesh count changed");
    require(output.value().cloth_rest_pose_receipts().size() == 1U,
        "published project must attach one cloth receipt to its derived mesh");

    const auto& [derived_mesh_id, stored_project_receipt] =
        *output.value().cloth_rest_pose_receipts().begin();
    require(derived_mesh_id != 1U, "cloth CLI overwrote the source mesh asset");
    const auto derived_mesh = output.value().meshes().find(derived_mesh_id);
    require(derived_mesh != output.value().meshes().end(),
        "project receipt points to a missing derived mesh");
    const std::vector<std::uint8_t> project_receipt_bytes(
        stored_project_receipt.bytes.begin(), stored_project_receipt.bytes.end());
    require(assets::sha256(std::span<const std::uint8_t>{project_receipt_bytes}) ==
            stored_project_receipt.digest,
        "project receipt digest does not bind its bytes");
    const auto rest_pose_receipt = production::ClothRestPoseBakeReceipt::deserialize(
        std::span<const std::uint8_t>{project_receipt_bytes});
    require(static_cast<bool>(rest_pose_receipt), "project cloth receipt cannot be decoded");

    auto ledger_text = std::ifstream(
        output_root / "receipts.carto-production-receipts", std::ios::binary);
    require(static_cast<bool>(ledger_text), "production receipt ledger is missing");
    const std::string ledger_bytes(
        (std::istreambuf_iterator<char>(ledger_text)), std::istreambuf_iterator<char>());
    const auto ledger = production::ProductionReceiptLedger::deserialize(ledger_bytes);
    require(static_cast<bool>(ledger) && ledger.value().size() == 2U,
        "production receipt ledger must contain the cloth simulation and rest-pose bake");

    const auto garment_record = output.value().garment_assets().find(1U);
    const auto source_mesh = output.value().meshes().find(1U);
    require(garment_record != output.value().garment_assets().end() &&
            source_mesh != output.value().meshes().end(),
        "published project lost its authored cloth source");
    const auto garment = production::GarmentAsset::deserialize(garment_record->second.bytes);
    require(static_cast<bool>(garment), "published GarmentAsset does not decode");

    assets::BlobStore blobs(output_root / "blobs");
    const production::ProductionArtifactReceipt* simulation_record = nullptr;
    const production::ProductionArtifactReceipt* rest_pose_record = nullptr;
    for (const auto& record : ledger.value().records()) {
        if (record.status != production::ReceiptStatus::admitted) continue;
        if (record.key.kind == production::ArtifactKind::cloth_simulation) {
            simulation_record = &record;
        } else if (record.key.kind == production::ArtifactKind::cloth_rest_pose) {
            rest_pose_record = &record;
        }
    }
    require(simulation_record != nullptr && rest_pose_record != nullptr,
        "published ledger is missing an admitted cloth product");
    const auto simulation_bytes = blobs.read(simulation_record->output_digest);
    const auto rest_pose_bytes = blobs.read(rest_pose_record->output_digest);
    require(static_cast<bool>(simulation_bytes) && static_cast<bool>(rest_pose_bytes),
        "published content-addressed cloth bytes are missing");
    const auto simulation = production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{simulation_bytes.value()});
    require(static_cast<bool>(simulation), "published cloth simulation does not decode");
    require(static_cast<bool>(simulation.value().validate(
                garment.value(), source_mesh->second)),
        "published cloth simulation does not bind the project source");
    require(rest_pose_receipt.value().simulation_artifact_digest ==
            simulation.value().artifact_digest,
        "rest-pose receipt does not bind the published simulation");
    const std::string project_receipt_payload(
        rest_pose_bytes.value().begin(), rest_pose_bytes.value().end());
    require(project_receipt_payload == stored_project_receipt.bytes,
        "project receipt and content-addressed receipt disagree");
    const auto rebuilt = production::bake_cloth_settled_rest_pose(
        simulation.value(), garment.value(), source_mesh->second);
    require(static_cast<bool>(rebuilt) && rebuilt.value().receipt.canonical() ==
            rest_pose_receipt.value().canonical(),
        "published rest-pose receipt does not reconstruct from the source simulation");
    const auto output_binding = production::bind_bake_source(
        rest_pose_receipt.value().output.identity, derived_mesh->second);
    require(static_cast<bool>(output_binding) &&
            output_binding.value() == rest_pose_receipt.value().output,
        "published derived mesh does not match its receipt output binding");
    require(std::filesystem::exists(output_root / "project.journal.log"),
        "published project transaction journal is missing");
    return 0;
}

int corrupt_project_receipt(const std::filesystem::path& project_path) {
    std::ifstream input(project_path, std::ios::binary);
    require(static_cast<bool>(input), "could not open the project receipt to corrupt");
    std::string encoded(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t record = encoded.find("CLOTH_REST_POSE_RECEIPT ");
    require(record != std::string::npos, "project receipt record is missing");
    const std::size_t separator = encoded.find(' ', record +
        std::string("CLOTH_REST_POSE_RECEIPT ").size());
    require(separator != std::string::npos && separator + 1U < encoded.size(),
        "project receipt digest field is missing");
    const std::size_t digest_character = separator + 1U;
    encoded[digest_character] = encoded[digest_character] == '0' ? '1' : '0';
    std::ofstream output(project_path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "could not write the corrupted project receipt");
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.close();
    require(static_cast<bool>(output), "could not close the corrupted project receipt");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "create") {
            return create_fixture(argv[2]);
        }
        if (argc == 3 && std::string_view(argv[1]) == "mutate") {
            return mutate_fixture(argv[2]);
        }
        if (argc == 4 && std::string_view(argv[1]) == "verify") {
            return verify_output(argv[2], argv[3]);
        }
        if (argc == 3 && std::string_view(argv[1]) == "corrupt-project-receipt") {
            return corrupt_project_receipt(argv[2]);
        }
        throw std::runtime_error("usage: cloth_cli_fixture create|verify ...");
    } catch (const std::exception& error) {
        std::cerr << "FAIL cloth CLI fixture: " << error.what() << '\n';
        return 1;
    }
}
