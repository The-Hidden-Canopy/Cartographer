#include <carto/production/closure.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

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

} // namespace

int main() {
    valid_receipt_round_trips_through_admission();
    stale_source_and_budget_fail_closed();
    failed_receipts_cannot_publish_output();
    ledger_never_falls_back_to_stale_evidence();
    ledger_round_trips_and_rejects_malformed_input();
    std::cout << "production closure tests passed\n";
}
