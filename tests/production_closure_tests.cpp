#include <carto/production/closure.hpp>
#include <carto/production/dependency.hpp>
#include <carto/production/artifact_store.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

namespace {

using carto::assets::Sha256Digest;

Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::production::ProductionArtifactReceipt admitted_receipt() {
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key.source = {"scene/house", carto::core::Revision{7U}, digest("source-v7")};
    receipt.key.kind = carto::production::ArtifactKind::render_mesh;
    receipt.key.form = carto::production::ExecutionForm::cached;
    receipt.key.algorithm = "surface-closure-v1";
    receipt.key.settings_digest = digest("settings");
    receipt.key.platform_profile = "cpu-reference";
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = digest("render-mesh-output");
    receipt.output_bytes = 4096U;
    receipt.quality = {.measured_error = 0.02, .importance = 0.9, .confidence = 0.98};
    receipt.cost = {.cook_milliseconds = 4.5,
                    .cpu_milliseconds = 0.6,
                    .gpu_milliseconds = 0.0,
                    .resident_bytes = 8192U,
                    .disk_bytes = 4096U};
    require(static_cast<bool>(receipt.refresh_digest()), "valid receipt refreshes its digest");
    return receipt;
}

void valid_receipt_round_trips_through_admission() {
    const auto receipt = admitted_receipt();
    require(static_cast<bool>(receipt.validate()), "valid receipt validates");
    const carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
    };
    require(static_cast<bool>(carto::production::validate_publication(
        receipt, receipt.key.source, policy)), "valid receipt is admitted");
}

void stale_source_and_budget_fail_closed() {
    auto receipt = admitted_receipt();
    auto stale_source = receipt.key.source;
    stale_source.revision = carto::core::Revision{8U};
    const carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
    };
    const auto stale_result = carto::production::validate_publication(
        receipt, stale_source, policy);
    require(!stale_result && stale_result.error().code == carto::core::ErrorCode::stale_data,
            "stale source is rejected before publication");

    auto bad_quality = receipt;
    bad_quality.quality.measured_error = 0.2;
    require(static_cast<bool>(bad_quality.refresh_digest()), "bad-quality receipt refreshes evidence");
    require(!carto::production::validate_publication(
        bad_quality, bad_quality.key.source, policy),
        "error budget rejects an admitted artifact");
}

void failed_receipts_cannot_publish_output() {
    auto receipt = admitted_receipt();
    receipt.status = carto::production::ReceiptStatus::failed;
    require(!receipt.refresh_digest(), "failed receipt with output is rejected");

    receipt.output_digest = {};
    receipt.output_bytes = 0U;
    require(static_cast<bool>(receipt.refresh_digest()), "failed receipt records no output");
    require(static_cast<bool>(receipt.validate()), "failed receipt remains valid evidence");
}

void ledger_never_falls_back_to_stale_evidence() {
    auto receipt = admitted_receipt();
    carto::production::ProductionReceiptLedger ledger;
    require(static_cast<bool>(ledger.append(receipt)), "admitted receipt enters ledger");
    auto found = ledger.query(receipt.key);
    require(found && found.value().has_value(), "admitted receipt is queryable");

    auto stale = receipt;
    stale.status = carto::production::ReceiptStatus::stale;
    stale.output_digest = {};
    stale.output_bytes = 0U;
    require(static_cast<bool>(stale.refresh_digest()), "stale receipt refreshes evidence");
    require(static_cast<bool>(ledger.append(stale)), "stale receipt enters ledger");
    found = ledger.query(receipt.key);
    require(found && !found.value().has_value(), "stale evidence masks older admission");
    require(static_cast<bool>(ledger.validate()), "ledger validates after stale evidence");
}

void ledger_round_trips_and_rejects_malformed_input() {
    auto receipt = admitted_receipt();
    carto::production::ProductionReceiptLedger ledger;
    require(static_cast<bool>(ledger.append(receipt)), "round-trip receipt enters ledger");
    const auto encoded = ledger.serialize();
    const auto restored = carto::production::ProductionReceiptLedger::deserialize(encoded);
    require(restored && restored.value().size() == 1U, "ledger round-trips one receipt");
    const auto found = restored.value().query(receipt.key);
    require(found && found.value().has_value(), "round-tripped receipt remains queryable");
    require(found.value()->receipt_digest == receipt.receipt_digest,
            "round-tripped receipt preserves evidence digest");

    const auto malformed = carto::production::ProductionReceiptLedger::deserialize(
        "CARTOGRAPHER_PRODUCTION_LEDGER_V1\nrecords 4097\n");
    require(!malformed, "oversized ledger is rejected before allocation");

    std::string oversized(carto::production::ProductionReceiptLedger::max_serialized_bytes + 1U, 'x');
    require(!carto::production::ProductionReceiptLedger::deserialize(oversized),
            "oversized serialized input is rejected before parsing");

    const auto trailing = carto::production::ProductionReceiptLedger::deserialize(
        encoded + "unexpected");
    require(!trailing, "trailing serialized data is rejected");
}

carto::production::ProductionCookKey cook_key(
    std::string identity,
    std::string token) {
    carto::production::ProductionCookKey key;
    key.source = {std::move(identity), carto::core::Revision{1U}, digest(token + "-source")};
    key.kind = carto::production::ArtifactKind::render_mesh;
    key.form = carto::production::ExecutionForm::cached;
    key.algorithm = "cook-v1";
    key.settings_digest = digest(token + "-settings");
    key.platform_profile = "cpu-reference";
    return key;
}

void cook_graph_is_deterministic_and_minimally_invalidating() {
    const auto source = cook_key("source", "source");
    const auto material = cook_key("material", "material");
    const auto mesh = cook_key("mesh", "mesh");
    const auto unrelated = cook_key("unrelated", "unrelated");
    const auto external = cook_key("external", "external");

    carto::production::ProductionCookGraph graph;
    require(static_cast<bool>(graph.add_node(source)), "source node enters cook graph");
    require(static_cast<bool>(graph.add_node(material, {source})),
            "material node enters cook graph");
    require(static_cast<bool>(graph.add_node(mesh, {material, external})),
            "mesh node enters cook graph with external input");
    require(static_cast<bool>(graph.add_node(unrelated)),
            "unrelated node enters cook graph");

    const auto order = graph.topological_order();
    require(order && order.value().size() == 4U, "cook graph has a full topological order");
    require(order.value()[0] == source && order.value()[1] == material &&
                order.value()[2] == mesh,
            "registered dependencies precede their derived products");

    const auto material_invalidation = graph.invalidation_order(material);
    require(material_invalidation && material_invalidation.value().size() == 2U,
            "material invalidation is bounded to dependent products");
    require(material_invalidation.value()[0] == material &&
                material_invalidation.value()[1] == mesh,
            "material invalidation is deterministic and downstream-only");

    const auto external_invalidation = graph.invalidation_order(external);
    require(external_invalidation && external_invalidation.value().size() == 1U &&
                external_invalidation.value()[0] == mesh,
            "external source invalidation reaches only direct dependents");

    carto::production::ProductionCookGraph cyclic;
    const auto left = cook_key("left", "left");
    const auto right = cook_key("right", "right");
    require(static_cast<bool>(cyclic.add_node(left, {right})),
            "incomplete graph may name an external dependency");
    require(!cyclic.add_node(right, {left}), "cycle is rejected when it becomes closed");
    require(cyclic.size() == 1U, "rejected cycle does not mutate the graph");

    carto::production::ProductionCookGraph invalid;
    require(!invalid.add_node(left, {left}), "self-dependency is rejected");
    require(!invalid.add_node(left, {right, right}), "duplicate dependency is rejected");
    require(invalid.size() == 0U, "rejected graph nodes do not mutate the graph");
}

void artifact_store_binds_bytes_to_admitted_receipts() {
    const auto root = std::filesystem::temp_directory_path() /
        "cartographer-production-closure-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    carto::assets::BlobStore blobs(root / "blobs");
    carto::production::ProductionArtifactStore store(blobs);

    const std::string payload_text = "derived-render-mesh";
    const auto payload = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(payload_text.data()), payload_text.size()};
    auto receipt = admitted_receipt();
    receipt.output_digest = digest(payload_text);
    receipt.output_bytes = payload.size();
    require(static_cast<bool>(receipt.refresh_digest()),
            "artifact receipt refreshes after binding its payload");
    const carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
    };
    require(static_cast<bool>(store.publish(receipt, payload, receipt.key.source, policy)),
            "matching derived bytes publish with their receipt");
    const auto restored = store.read(receipt.key, receipt.key.source, policy);
    require(restored && std::string(reinterpret_cast<const char*>(restored.value().data()),
                                    restored.value().size()) == payload_text,
            "admitted derived bytes reopen through the receipt key");

    const auto wrong_payload = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("tampered"), 8U};
    require(!store.publish(receipt, wrong_payload, receipt.key.source, policy),
            "payload digest mismatch is rejected before publication");
    require(store.ledger().size() == 1U,
            "rejected payload does not append evidence");

    auto stale_source = receipt.key.source;
    stale_source.revision = carto::core::Revision{2U};
    const auto stale_read = store.read(receipt.key, stale_source, policy);
    require(!stale_read && stale_read.error().code == carto::core::ErrorCode::stale_data,
            "stale source cannot read an admitted derived artifact");

    std::filesystem::remove_all(root, cleanup_error);
}

} // namespace

int main() {
    valid_receipt_round_trips_through_admission();
    stale_source_and_budget_fail_closed();
    failed_receipts_cannot_publish_output();
    ledger_never_falls_back_to_stale_evidence();
    ledger_round_trips_and_rejects_malformed_input();
    cook_graph_is_deterministic_and_minimally_invalidating();
    artifact_store_binds_bytes_to_admitted_receipts();
    std::cout << "production closure tests passed\n";
}
