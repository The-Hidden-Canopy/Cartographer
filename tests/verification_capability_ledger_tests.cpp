#include <carto/verification/capability_ledger.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/production/prepared_scene.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

using carto::assets::Sha256Digest;
using carto::verification::CapabilityClaim;
using carto::verification::CapabilityLedger;
using carto::verification::CapabilityNonClaim;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

CapabilityClaim claim(
    std::string feature,
    carto::production::PreparedCapabilityEvidence evidence,
    bool required,
    const Sha256Digest& receipt) {
    CapabilityClaim value;
    value.feature = std::move(feature);
    value.evidence = evidence;
    value.required = required;
    value.admission_receipt_digest = receipt;
    value.detail = "verified against the admitted prepared product";
    return value;
}

CapabilityNonClaim non_claim(std::string feature, const Sha256Digest& receipt) {
    CapabilityNonClaim value;
    value.feature = std::move(feature);
    value.admission_receipt_digest = receipt;
    value.reason = "no neutral prepared product provides this capability";
    return value;
}

void claims_and_non_claims_record_truthful_verdicts() {
    using carto::production::PreparedCapabilityEvidence;
    const auto receipt = digest("admission-receipt-one");
    CapabilityLedger ledger;

    require(static_cast<bool>(ledger.claim(
                claim("scene.geometry", PreparedCapabilityEvidence::derived_product,
                    true, receipt))),
        "a supported feature enters the ledger as a claim");
    require(static_cast<bool>(ledger.non_claim(
                non_claim("scene.destruction", receipt))),
        "an unsupported feature enters the ledger as an explicit non-claim");
    require(ledger.size() == 2U, "ledger counts both record kinds");

    const auto supported = ledger.query("scene.geometry");
    require(supported.has_value() && supported->declared() &&
            supported->currently_supported() && supported->latest_claim.has_value() &&
            !supported->latest_non_claim.has_value() &&
            supported->latest_claim->admission_receipt_digest == receipt,
        "query binds a supported feature to its claim and admission receipt");

    const auto unsupported = ledger.query("scene.destruction");
    require(unsupported.has_value() && unsupported->declared() &&
            !unsupported->currently_supported() &&
            unsupported->latest_non_claim.has_value() &&
            !unsupported->latest_claim.has_value(),
        "query shows what is NOT supported instead of staying silent");
    require(!ledger.query("scene.never-asked").has_value(),
        "an undeclared feature reads back as absent, distinct from a non-claim");

    // A later claim supersedes an earlier non-claim and vice versa; the
    // append-only history is never rewritten.
    require(static_cast<bool>(ledger.claim(
                claim("scene.destruction", PreparedCapabilityEvidence::derived_product,
                    false, receipt))),
        "a later claim may supersede a non-claim");
    const auto superseded = ledger.query("scene.destruction");
    require(superseded.has_value() && superseded->currently_supported() &&
            superseded->latest_claim.has_value() &&
            superseded->latest_non_claim.has_value() &&
            superseded->latest_claim->sequence > superseded->latest_non_claim->sequence,
        "the latest record wins while both sides stay inspectable");
    require(ledger.non_claims().size() == 1U,
        "the superseded non-claim remains in append-only history");

    require(static_cast<bool>(ledger.non_claim(
                non_claim("scene.geometry", receipt))),
        "a later non-claim may supersede a claim");
    const auto reverted = ledger.query("scene.geometry");
    require(reverted.has_value() && !reverted->currently_supported() &&
            reverted->latest_non_claim->sequence > reverted->latest_claim->sequence,
        "a newer non-claim flips the verdict back to unsupported");
}

void record_prepared_capabilities_splits_truthfully() {
    using carto::production::PreparedCapabilityEvidence;
    using carto::production::PreparedCapabilityResult;
    const auto receipt = digest("admission-receipt-corpus");
    const std::vector<PreparedCapabilityResult> capabilities{
        {"scene.geometry", PreparedCapabilityEvidence::derived_product, true,
         "geometry derived as an exact neutral product"},
        {"scene.destruction", PreparedCapabilityEvidence::unsupported, false,
         "no derived product exists for authored destruction intent"},
    };

    CapabilityLedger ledger;
    require(static_cast<bool>(
                ledger.record_prepared_capabilities(capabilities, receipt)),
        "prepared capability results record into the ledger");
    require(ledger.claims().size() == 1U && ledger.non_claims().size() == 1U,
        "unsupported evidence becomes an explicit non-claim");
    require(ledger.claims().front().sequence == 1U &&
            ledger.non_claims().front().sequence == 2U,
        "mixed batches keep one global append sequence");
    require(ledger.claims().front().admission_receipt_digest == receipt &&
            ledger.non_claims().front().admission_receipt_digest == receipt,
        "every converted record binds the same admission receipt");

    const auto geometry = ledger.query("scene.geometry");
    const auto destruction = ledger.query("scene.destruction");
    require(geometry.has_value() && geometry->currently_supported(),
        "converted claim reads back supported");
    require(destruction.has_value() && !destruction->currently_supported() &&
            destruction->latest_non_claim.has_value(),
        "converted non-claim reads back explicitly unsupported");

    CapabilityLedger full;
    require(!full.record_prepared_capabilities(capabilities, Sha256Digest{}),
        "recording refuses a zero admission receipt digest");
}

void ledger_round_trip_and_digest_are_deterministic() {
    using carto::production::PreparedCapabilityEvidence;
    const auto first_receipt = digest("admission-receipt-alpha");
    const auto second_receipt = digest("admission-receipt-beta");
    CapabilityLedger ledger;
    require(static_cast<bool>(ledger.claim(
                claim("scene.geometry", PreparedCapabilityEvidence::derived_product,
                    true, first_receipt))),
        "first fixture claim appends");
    require(static_cast<bool>(ledger.non_claim(
                non_claim("scene.destruction", first_receipt))),
        "first fixture non-claim appends");
    require(static_cast<bool>(ledger.claim(
                claim("scene.material-authoring",
                    PreparedCapabilityEvidence::preserved_source, true,
                    second_receipt))),
        "second fixture claim appends");

    const auto text = ledger.serialize();
    const auto reopened = CapabilityLedger::deserialize(text);
    require(reopened && reopened.value().serialize() == text,
        "capability ledger survives a deterministic neutral round trip");
    const auto first_digest = ledger.canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value() &&
            first_digest.value() == digest(text),
        "capability ledger identity survives reopen");

    CapabilityLedger rebuilt;
    require(static_cast<bool>(rebuilt.claim(
                claim("scene.geometry", PreparedCapabilityEvidence::derived_product,
                    true, first_receipt))) &&
            static_cast<bool>(rebuilt.non_claim(
                non_claim("scene.destruction", first_receipt))) &&
            static_cast<bool>(rebuilt.claim(
                claim("scene.material-authoring",
                    PreparedCapabilityEvidence::preserved_source, true,
                    second_receipt))),
        "identical append sequence rebuilds the ledger");
    const auto rebuilt_digest = rebuilt.canonical_digest();
    require(rebuilt_digest && rebuilt_digest.value() == first_digest.value(),
        "same admitted truth yields the same ledger digest");
}

void invalid_records_fail_closed() {
    using carto::production::PreparedCapabilityEvidence;
    const auto receipt = digest("admission-receipt-guard");
    CapabilityLedger ledger;

    auto unsupported_claim = claim("scene.geometry",
        PreparedCapabilityEvidence::unsupported, true, receipt);
    require(!ledger.claim(std::move(unsupported_claim)),
        "a claim may never carry unsupported evidence");
    require(ledger.claims().empty(), "rejected claim leaves no history behind");

    auto zero_receipt = claim("scene.geometry",
        PreparedCapabilityEvidence::derived_product, true, Sha256Digest{});
    require(!ledger.claim(std::move(zero_receipt)),
        "a claim without an admission receipt is rejected");

    auto empty_feature = claim("",
        PreparedCapabilityEvidence::derived_product, true, receipt);
    require(!ledger.claim(std::move(empty_feature)),
        "an empty feature is rejected");

    auto oversized_feature = claim(std::string(300U, 'x'),
        PreparedCapabilityEvidence::derived_product, true, receipt);
    require(!ledger.claim(std::move(oversized_feature)),
        "an oversized feature is rejected");

    auto empty_reason = non_claim("scene.destruction", receipt);
    empty_reason.reason.clear();
    require(!ledger.non_claim(std::move(empty_reason)),
        "a non-claim without a reason is rejected");

    require(ledger.size() == 0U, "fail-closed appends leave the ledger empty");
}

void hostile_serialization_fails_closed() {
    CapabilityLedger empty;
    const auto text = empty.serialize();
    require(text == "CARTOGRAPHER_CAPABILITY_LEDGER 1\n"
                    "CLAIMS 0\n"
                    "NON_CLAIMS 0\n"
                    "END\n",
        "empty ledger serializes to its canonical form");

    std::string future = text;
    future.replace(0U, std::string{"CARTOGRAPHER_CAPABILITY_LEDGER 1"}.size(),
        "CARTOGRAPHER_CAPABILITY_LEDGER 99");
    const auto future_result = CapabilityLedger::deserialize(future);
    require(!future_result &&
            future_result.error().code == carto::core::ErrorCode::version_mismatch,
        "unknown future ledger versions fail precisely");

    require(!CapabilityLedger::deserialize(text + "trailing"),
        "trailing unbound data is rejected");

    std::string oversized = text;
    oversized.replace(oversized.find("CLAIMS 0"), std::string{"CLAIMS 0"}.size(),
        "CLAIMS 99999");
    require(!CapabilityLedger::deserialize(oversized),
        "hostile collection counts are rejected before allocation");

    const auto receipt = digest("admission-receipt-hostile").hex();
    const std::string duplicated =
        "CARTOGRAPHER_CAPABILITY_LEDGER 1\n"
        "CLAIMS 2\n"
        "CLAIM 1 \"scene.a\" 1 1 " + receipt + " \"detail\"\n"
        "CLAIM 1 \"scene.b\" 1 0 " + receipt + " \"detail\"\n"
        "NON_CLAIMS 0\n"
        "END\n";
    require(!CapabilityLedger::deserialize(duplicated),
        "duplicated record sequences are rejected");

    const std::string zero_digest_claim =
        "CARTOGRAPHER_CAPABILITY_LEDGER 1\n"
        "CLAIMS 1\n"
        "CLAIM 1 \"scene.a\" 1 1 " +
        std::string(64U, '0') + " \"detail\"\n"
        "NON_CLAIMS 0\n"
        "END\n";
    require(!CapabilityLedger::deserialize(zero_digest_claim),
        "a zero admission receipt digest fails closed");
}

} // namespace

int main() {
    claims_and_non_claims_record_truthful_verdicts();
    record_prepared_capabilities_splits_truthfully();
    ledger_round_trip_and_digest_are_deterministic();
    invalid_records_fail_closed();
    hostile_serialization_fails_closed();
    std::cout << "verification capability ledger tests passed\n";
    return EXIT_SUCCESS;
}
