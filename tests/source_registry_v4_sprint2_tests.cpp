#include <carto/assets/source_registry.hpp>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace carto;

void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest digest(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
assets::SourceImportRecord basic() {
    assets::SourceImportRecord record;
    record.identity = "source.one";
    record.relative_source_path = "sources/one.obj";
    record.source_generation = core::Revision{1U};
    record.kind = assets::SourceAssetKind::mesh;
    record.source_digest = digest("fixture source");
    record.importer_identity = "carto.import.obj";
    record.artifact_identity = "mesh.one";
    record.artifact_digest = digest("fixture geometry");
    record.options.weld_tolerance_meters = 0.0;
    return record;
}
assets::SourceImportRecord document() {
    auto record = basic();
    record.kind = assets::SourceAssetKind::other;
    record.importer_identity = "carto.import.gltf-scene";
    record.artifact_identity = "gltf.scene.one";
    using K = assets::SourceDocumentProductKind;
    record.document_products = {{K::mesh,1U,11U}, {K::mesh,2U,12U},
        {K::node,1U,11U}, {K::material,1U,21U}, {K::texture,1U,31U}, {K::sampler,1U,41U}};
    record.artifact_digest = assets::source_document_product_digest(record.source_digest, record.document_products);
    return record;
}
std::string quote(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}
std::string common(const assets::SourceImportRecord& r, unsigned version) {
    return "SOURCE_IMPORT_RECORD " + std::to_string(version) + "\nIDENTITY " + quote(r.identity) +
        " " + quote(r.relative_source_path) + " " + std::to_string(r.source_generation.value()) +
        " " + std::to_string(static_cast<unsigned>(r.kind)) + "\nSOURCE " + r.source_digest.hex() +
        "\nIMPORTER " + quote(r.importer_identity) + " " + std::to_string(r.importer_version) +
        "\nOPTIONS 1 1 1 0 1 1 1 0 0\nARTIFACT " + quote(r.artifact_identity) + " " +
        r.artifact_digest.hex() + "\nDOMAINS " + std::to_string(r.domain_digests.size()) + "\n";
}
void exact_round_trip(const assets::SourceImportRecord& record, const std::string& golden) {
    require(static_cast<bool>(record.validate()), "fixture validates");
    require(record.serialize() == golden, "record bytes match fixed canonical framing");
    const auto parsed = assets::SourceImportRecord::deserialize(golden);
    require(parsed && parsed.value().serialize() == golden && digest(parsed.value().serialize()) == digest(golden),
        "record and digest survive canonical decode/encode");
}
void registry_round_trip(const assets::SourceImportRecord& record) {
    assets::ProjectAssetRegistry registry;
    require(static_cast<bool>(registry.set_record(record)), "record enters registry");
    const auto encoded = registry.serialize();
    const auto digest_before = registry.canonical_digest();
    const auto reopened = assets::ProjectAssetRegistry::deserialize(encoded);
    require(digest_before && reopened && reopened.value().serialize() == encoded &&
            reopened.value().canonical_digest().value() == digest_before.value(),
        "legacy or v4 registry round trip preserves bytes and digest");
}
void legacy_and_v4_bytes() {
    auto v1 = basic();
    exact_round_trip(v1, common(v1, 1U) + "END\n");
    registry_round_trip(v1);

    auto v2 = basic();
    v2.importer_identity = "carto.import.generated-gltf";
    botanical::Source source;
    source.identity = "tree";
    source.revision = core::Revision{1U};
    const auto snapshot = source.serialize();
    v2.botanical_generation = assets::BotanicalGenerationReceipt{
        snapshot, digest("job"), digest(snapshot), v2.source_digest};
    v2.domain_digests = {{assets::SourceAssetDomain::geometry, v2.artifact_digest}};
    exact_round_trip(v2, common(v2, 2U) + "DOMAIN 0 " + v2.artifact_digest.hex() +
        "\nBOTANICAL_GENERATION " + quote(v2.botanical_generation->serialize()) + "\nEND\n");
    registry_round_trip(v2);

    auto v3 = basic();
    v3.authored_tags = {"approved", "reviewed"};
    v3.dependencies = {"source.parent"};
    exact_round_trip(v3, common(v3, 3U) +
        "AUTHORED_TAGS 2\nTAG \"approved\"\nTAG \"reviewed\"\nDEPENDENCIES 1\n"
        "DEPENDENCY \"source.parent\"\nEND\n");
    auto v3_receipt = v2;
    v3_receipt.authored_tags = {"reviewed"};
    exact_round_trip(v3_receipt, common(v3_receipt, 3U) + "DOMAIN 0 " +
        v3_receipt.artifact_digest.hex() +
        "\nAUTHORED_TAGS 1\nTAG \"reviewed\"\nDEPENDENCIES 0\nBOTANICAL_GENERATION " +
        quote(v3_receipt.botanical_generation->serialize()) + "\nEND\n");
    registry_round_trip(v3_receipt);

    auto v4 = document();
    const std::string mapping = "CARTOGRAPHER_SOURCE_DOCUMENT_PRODUCTS 1\n" + v4.source_digest.hex() +
        "\n6\n0 1 11\n0 2 12\n1 1 11\n2 1 21\n3 1 31\n4 1 41\n";
    require(v4.artifact_digest == digest(mapping), "v4 digest binds exact source and ordered typed mapping");
    exact_round_trip(v4, common(v4, 4U) +
        "AUTHORED_TAGS 0\nDEPENDENCIES 0\nDOCUMENT_PRODUCTS 6\n"
        "PRODUCT 0 1 11\nPRODUCT 0 2 12\nPRODUCT 1 1 11\nPRODUCT 2 1 21\n"
        "PRODUCT 3 1 31\nPRODUCT 4 1 41\nEND\n");
    registry_round_trip(v4);
}
void corrupt_and_duplicate_bindings_refuse() {
    const auto original = document();
    const auto encoded = original.serialize();
    auto changed = original;
    changed.document_products[0].product_id++;
    require(!changed.validate(), "changed mapping with stale digest refuses");
    changed = original;
    changed.document_products[1].product_id = changed.document_products[0].product_id;
    changed.artifact_digest = assets::source_document_product_digest(changed.source_digest, changed.document_products);
    require(!changed.validate(), "duplicate target within one namespace refuses even with recomputed digest");
    changed = original;
    changed.document_products[1].source_index = 1U;
    changed.artifact_digest = assets::source_document_product_digest(changed.source_digest, changed.document_products);
    require(!changed.validate(), "duplicate source index refuses even with recomputed digest");
    changed = original;
    changed.document_products[1].source_index = 3U;
    changed.artifact_digest = assets::source_document_product_digest(changed.source_digest, changed.document_products);
    require(!changed.validate(), "source index gap refuses");
    changed = original;
    changed.document_products.pop_back();
    require(!changed.validate(), "removed product without new digest refuses");
    changed = original;
    changed.source_digest = digest("different source bytes");
    require(!changed.validate(), "changed source with stale mapping digest refuses");
    changed = original;
    changed.document_products.erase(changed.document_products.begin() + 3);
    changed.artifact_digest = assets::source_document_product_digest(changed.source_digest, changed.document_products);
    require(!changed.validate(), "mapping without required material refuses even with new digest");
    changed.importer_version = 2U;
    require(bool(changed.validate()), "projected-material importer can bind a scene without authored materials");
    const auto projected = assets::SourceImportRecord::deserialize(changed.serialize());
    require(projected && projected.value().importer_version == 2U &&
            projected.value().document_products == changed.document_products,
            "projected-material importer lineage survives record round trip");
    registry_round_trip(changed);
    assets::ProjectAssetRegistry projected_registry;
    require(bool(projected_registry.set_record(changed)), "projected record enters registry");
    auto future = changed;
    future.importer_version = 3U;
    const auto future_record = assets::SourceImportRecord::deserialize(future.serialize());
    require(!future_record && future_record.error().code == core::ErrorCode::version_mismatch,
            "future glTF lineage version refuses with a version diagnostic");
    auto future_registry = projected_registry.serialize();
    const auto encoded_record = quote(changed.serialize());
    const auto record_position = future_registry.find(encoded_record);
    require(record_position != std::string::npos, "projected registry contains its exact record");
    future_registry.replace(record_position, encoded_record.size(), quote(future.serialize()));
    const auto future_registry_result = assets::ProjectAssetRegistry::deserialize(future_registry);
    require(!future_registry_result && future_registry_result.error().code == core::ErrorCode::version_mismatch &&
            future_registry_result.error().message.find("maximum 2") != std::string::npos,
            "registry preserves future lineage version diagnostic");
    auto future_schema = changed.serialize();
    const auto schema_position = future_schema.find("SOURCE_IMPORT_RECORD 4\n");
    require(schema_position == 0U, "projected record has the expected schema header");
    future_schema.replace(schema_position, std::string("SOURCE_IMPORT_RECORD 4\n").size(),
                          "SOURCE_IMPORT_RECORD 5\n");
    const auto future_schema_result = assets::SourceImportRecord::deserialize(future_schema);
    require(!future_schema_result && future_schema_result.error().code == core::ErrorCode::version_mismatch,
            "future record schema refuses with a version diagnostic");
    auto malformed = encoded;
    const auto product = malformed.find("PRODUCT 0 1 11\n");
    require(product != std::string::npos, "fixture product exists");
    malformed.replace(product, std::string("PRODUCT 0 1 11\n").size(), "PRODUCT 255 1 11\n");
    require(!assets::SourceImportRecord::deserialize(malformed), "unknown product kind refuses");
    require(!assets::SourceImportRecord::deserialize(encoded + "EXTRA\n"), "trailing field refuses");
    malformed = encoded;
    const auto end = malformed.rfind("END\n");
    malformed.insert(end, "DEPENDENCIES 0\n");
    require(!assets::SourceImportRecord::deserialize(malformed), "duplicate field refuses");
    require(!assets::SourceImportRecord::deserialize(encoded.substr(0, encoded.size() - 5U)),
        "truncated end marker refuses");
    malformed = encoded;
    const auto header = malformed.find("DOCUMENT_PRODUCTS 6\n");
    require(header != std::string::npos, "fixture table exists");
    malformed.replace(header, std::string("DOCUMENT_PRODUCTS 6\n").size(), "DOCUMENT_PRODUCTS 0\n");
    require(!assets::SourceImportRecord::deserialize(malformed), "empty v4 table refuses");
}
void registry_identity_and_lineage() {
    auto first = document();
    auto duplicate = first;
    duplicate.identity = "source.two";
    duplicate.relative_source_path = "sources/two.gltf";
    duplicate.artifact_identity = "gltf.scene.two";
    assets::ProjectAssetRegistry registry;
    require(static_cast<bool>(registry.set_record(first)), "first source admitted");
    const auto before = registry.serialize();
    require(!registry.set_record(duplicate) && registry.serialize() == before,
        "cross-source product collision refuses atomically");
    const auto conflict = assets::plan_source_reimport(registry, {duplicate});
    require(conflict && conflict.value().entries.size() == 2U &&
            conflict.value().entries[0].change == assets::SourceReimportChange::identity_conflict &&
            conflict.value().entries[1].change == assets::SourceReimportChange::identity_conflict &&
            !assets::apply_source_reimport_plan(registry, conflict.value()) && registry.serialize() == before,
        "reimport explicitly reports cross-source product identity conflict without adoption");
    require(!assets::plan_source_reimport(registry, {first, duplicate}),
        "two incoming records cannot claim the same product");
    const auto reopened = assets::ProjectAssetRegistry::deserialize(before);
    require(reopened && reopened.value().serialize() == before &&
            reopened.value().canonical_digest().value() == registry.canonical_digest().value(),
        "registry bytes and canonical digest survive reopen");
    std::ostringstream forged;
    forged << "CARTOGRAPHER_SOURCE_ASSET_REGISTRY 1\nRECORDS 2\nRECORD " << quote(first.identity)
           << ' ' << quote(first.serialize()) << "\nRECORD " << quote(duplicate.identity) << ' '
           << quote(duplicate.serialize()) << "\nLINEAGE 2\nLINEAGE_RECORD " << quote(first.identity)
           << " 1\nLINEAGE_RECORD " << quote(duplicate.identity) << " 1\nEND\n";
    require(!assets::ProjectAssetRegistry::deserialize(forged.str()), "cross-source product collision refuses on readback");
    require(static_cast<bool>(registry.erase_record(first.identity, first.source_generation)), "source removal succeeds");
    const auto tombstone = registry.serialize();
    auto retained = assets::ProjectAssetRegistry::deserialize(tombstone);
    require(retained && retained.value().lineage().at(first.identity) == core::Revision{1U},
        "tombstone survives reopen");
    require(!retained.value().set_record(first), "old generation cannot resurrect a removed source");
    first.source_generation = core::Revision{2U};
    require(static_cast<bool>(retained.value().set_record(first)), "next generation can be restored");
    require(!assets::ProjectAssetRegistry::deserialize(retained.value().serialize() + "UNKNOWN\n"),
        "registry trailing fields refuse");
}
void serialized_record_limits_are_symmetric() {
    auto large = document();
    large.document_products.clear();
    using K = assets::SourceDocumentProductKind;
    for (std::uint64_t index = 1U; index <= 8190U; ++index) {
        large.document_products.push_back({K::mesh, index, std::numeric_limits<std::uint64_t>::max() - index});
    }
    large.document_products.push_back({K::node, 1U, 1U});
    large.document_products.push_back({K::material, 1U, 1U});
    large.artifact_digest = assets::source_document_product_digest(large.source_digest, large.document_products);
    require(large.serialize().size() > assets::SourceImportRecord::kMaxSerializedBytes,
        "fixture exceeds the existing decoder bound");
    assets::ProjectAssetRegistry registry;
    const auto before = registry.serialize();
    require(!large.validate() && !registry.set_record(large) && registry.serialize() == before,
        "oversized record cannot enter registry or change its lineage");
    large.document_products.erase(large.document_products.begin() + 3000, large.document_products.end() - 2);
    large.artifact_digest = assets::source_document_product_digest(large.source_digest, large.document_products);
    require(large.serialize().size() <= assets::SourceImportRecord::kMaxSerializedBytes &&
            large.validate() && registry.set_record(large), "bounded v4 mapping remains admitted");
    const auto reopened = assets::ProjectAssetRegistry::deserialize(registry.serialize());
    require(reopened && reopened.value().serialize() == registry.serialize(), "admitted bounded mapping reopens");
}
} // namespace
int main() {
    legacy_and_v4_bytes();
    corrupt_and_duplicate_bindings_refuse();
    registry_identity_and_lineage();
    serialized_record_limits_are_symmetric();
    std::cout << "source registry v4 sprint2 tests passed\n";
    return EXIT_SUCCESS;
}
