#include <carto/assets/source_registry.hpp>

#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace carto;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest digest(std::string_view s) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}
assets::SourceImportRecord basic(std::string suffix) {
    assets::SourceImportRecord record;
    record.identity = "source." + suffix;
    record.relative_source_path = "sources/" + suffix + ".obj";
    record.source_generation = core::Revision{1U};
    record.kind = assets::SourceAssetKind::mesh;
    record.source_digest = digest("source bytes");
    record.importer_identity = "carto.import.obj";
    record.artifact_identity = "mesh." + suffix;
    record.artifact_digest = digest("mesh bytes");
    record.options.weld_tolerance_meters = 0.0;
    return record;
}
assets::SourceImportRecord scene(std::size_t meshes) {
    auto record = basic("scene");
    record.kind = assets::SourceAssetKind::other;
    record.importer_identity = "carto.import.gltf-scene";
    record.artifact_identity = "gltf.scene";
    using K = assets::SourceDocumentProductKind;
    for (std::size_t i = 1; i <= meshes; ++i) {
        record.document_products.push_back({K::mesh, static_cast<std::uint64_t>(i),
            std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(i)});
    }
    record.document_products.push_back({K::node, 1U, 1U});
    record.document_products.push_back({K::material, 1U, 1U});
    record.artifact_digest = assets::source_document_product_digest(record.source_digest, record.document_products);
    return record;
}
void record_size_and_product_budget() {
    auto oversized = scene(8190U);
    require(oversized.document_products.size() == assets::SourceImportRecord::kMaxDocumentProducts &&
            oversized.serialize().size() > assets::SourceImportRecord::kMaxSerializedBytes,
        "maximum product count may exceed record byte limit");
    assets::ProjectAssetRegistry registry;
    const auto before = registry.serialize();
    require(!oversized.validate() && !registry.set_record(oversized) && registry.serialize() == before,
        "oversized record refuses before record or lineage mutation");
    auto too_many = oversized;
    too_many.document_products.push_back({assets::SourceDocumentProductKind::sampler, 1U, 2U});
    require(!too_many.validate(), "product count above existing bound refuses");
    auto bounded = scene(6000U);
    require(bounded.serialize().size() <= assets::SourceImportRecord::kMaxSerializedBytes &&
            bounded.validate() && registry.set_record(bounded), "large bounded mapping remains valid");
    const auto reopened = assets::ProjectAssetRegistry::deserialize(registry.serialize());
    require(reopened && reopened.value().serialize() == registry.serialize(), "bounded mapping round-trips");
    auto overflow_text = bounded.serialize();
    const auto product = overflow_text.find("PRODUCT 0 1 ");
    require(product != std::string::npos, "product fixture exists");
    const auto end = overflow_text.find('\n', product);
    overflow_text.replace(product, end - product, "PRODUCT 0 1 18446744073709551616");
    require(!assets::SourceImportRecord::deserialize(overflow_text),
        "product ID outside uint64 range refuses");
}
void incoming_aggregate_budget() {
    auto exemplar = scene(6000U);
    require(static_cast<bool>(exemplar.validate()), "aggregate fixture is individually valid");
    std::vector<assets::SourceImportRecord> incoming;
    std::size_t bytes = 0U;
    while (bytes <= assets::ProjectAssetRegistry::kMaxAggregateRecordBytes) {
        auto record = exemplar;
        const auto suffix = std::to_string(incoming.size());
        record.identity = "source.s" + suffix;
        record.relative_source_path = "sources/s" + suffix + ".gltf";
        record.artifact_identity = "gltf.s" + suffix;
        bytes += record.serialize().size();
        incoming.push_back(std::move(record));
    }
    assets::ProjectAssetRegistry empty;
    const auto before = empty.serialize();
    const auto planned = assets::plan_source_reimport(empty, std::move(incoming));
    require(!planned && planned.error().code == core::ErrorCode::validation_failed &&
            empty.serialize() == before, "over-budget incoming set refuses without mutation");
}
void conflicts_and_lineage_are_atomic() {
    assets::ProjectAssetRegistry registry;
    auto first = basic("one");
    auto second = basic("two");
    require(static_cast<bool>(registry.set_record(first)) && static_cast<bool>(registry.set_record(second)),
        "independent sources admitted");
    auto third = basic("three");
    third.relative_source_path = second.relative_source_path;
    const auto before = registry.serialize();
    const auto conflict = assets::plan_source_reimport(registry, {first, third});
    require(conflict && conflict.value().entries.size() == 3U &&
            conflict.value().entries[1].change == assets::SourceReimportChange::identity_conflict &&
            conflict.value().entries[2].change == assets::SourceReimportChange::identity_conflict &&
            !assets::apply_source_reimport_plan(registry, conflict.value()) && registry.serialize() == before,
        "late path conflict retains both owners and lineage");
    require(static_cast<bool>(registry.erase_record(first.identity, first.source_generation)), "remove source");
    const auto tombstone = registry.serialize();
    require(!registry.set_record(first) && registry.serialize() == tombstone,
        "stale generation refuses without changing tombstone");
    first.source_generation = core::Revision{2U};
    require(static_cast<bool>(registry.set_record(first)), "next generation admitted");
}
void legacy_serialization() {
    auto v1 = basic("legacy");
    const std::string expected = "SOURCE_IMPORT_RECORD 1\nIDENTITY \"source.legacy\" \"sources/legacy.obj\" 1 0\nSOURCE " +
        v1.source_digest.hex() + "\nIMPORTER \"carto.import.obj\" 1\nOPTIONS 1 1 1 0 1 1 1 0 0\n"
        "ARTIFACT \"mesh.legacy\" " + v1.artifact_digest.hex() + "\nDOMAINS 0\nEND\n";
    require(v1.serialize() == expected && v1.validate(), "v1 exact bytes retained");
    auto v2 = v1;
    v2.importer_identity = "carto.import.generated-gltf";
    botanical::Source plant;
    plant.identity = "tree";
    plant.revision = core::Revision{1U};
    const auto snapshot = plant.serialize();
    v2.botanical_generation = assets::BotanicalGenerationReceipt{
        snapshot, digest("job"), digest(snapshot), v2.source_digest};
    v2.domain_digests = {{assets::SourceAssetDomain::geometry, v2.artifact_digest}};
    std::ostringstream receipt;
    receipt << std::quoted(v2.botanical_generation->serialize());
    const std::string expected_v2 = "SOURCE_IMPORT_RECORD 2\nIDENTITY \"source.legacy\" \"sources/legacy.obj\" 1 0\nSOURCE " +
        v2.source_digest.hex() + "\nIMPORTER \"carto.import.generated-gltf\" 1\nOPTIONS 1 1 1 0 1 1 1 0 0\n"
        "ARTIFACT \"mesh.legacy\" " + v2.artifact_digest.hex() + "\nDOMAINS 1\nDOMAIN 0 " +
        v2.artifact_digest.hex() + "\nBOTANICAL_GENERATION " + receipt.str() + "\nEND\n";
    const auto parsed_v2 = assets::SourceImportRecord::deserialize(expected_v2);
    require(v2.validate() && v2.serialize() == expected_v2 && parsed_v2 &&
            parsed_v2.value().serialize() == expected_v2, "v2 botanical framing retained");
    auto v3 = v1;
    v3.authored_tags = {"reviewed"};
    const auto v3_bytes = v3.serialize();
    const auto reopened = assets::SourceImportRecord::deserialize(v3_bytes);
    require(v3_bytes.starts_with("SOURCE_IMPORT_RECORD 3\n") && reopened &&
            reopened.value().serialize() == v3_bytes && digest(reopened.value().serialize()) == digest(v3_bytes),
        "v3 bytes and digest retained");
    auto v4 = scene(2U);
    const auto v4_bytes = v4.serialize();
    const auto decoded = assets::SourceImportRecord::deserialize(v4_bytes);
    require(v4_bytes.starts_with("SOURCE_IMPORT_RECORD 4\n") && decoded &&
            decoded.value().serialize() == v4_bytes && decoded.value().artifact_digest == v4.artifact_digest,
        "v4 product binding bytes and digest retained");
}
} // namespace
int main() {
    record_size_and_product_budget();
    incoming_aggregate_budget();
    conflicts_and_lineage_are_atomic();
    legacy_serialization();
    std::cout << "source registry budget sprint3 tests passed\n";
    return EXIT_SUCCESS;
}
