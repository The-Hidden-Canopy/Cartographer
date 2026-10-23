#include <carto/verification/donor_promotion.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

namespace {

using carto::verification::DonorClass;
using carto::verification::DonorEvidenceStatus;
using carto::verification::DonorPromotionLedger;
using carto::verification::DonorPromotionReceipt;
using carto::verification::PromotionDisposition;
using carto::verification::PromotionStatus;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest_text(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

DonorPromotionReceipt observed_architecture_record() {
    DonorPromotionReceipt record;
    record.promotion_id = "external-editor-camera-pattern";
    record.target_module = "editor_interaction";
    record.donor_class = DonorClass::borrow_pattern;
    record.disposition = PromotionDisposition::architecture_only;
    record.status = PromotionStatus::observed;
    record.donor.status = DonorEvidenceStatus::privately_verified;
    record.license_basis = "owner-reviewed donor metadata; no source copied";
    record.semantic_changes = {
        "rebound camera state to Cartographer authoring identity",
        "removed donor-specific runtime assumptions",
    };
    record.removed_authorities = {
        "runtime authority remains outside this ledger",
        "source mutation remains behind the Cartographer transaction path",
    };
    record.tests = {"cartographer_viewport_interaction_tests"};
    record.fixtures = {"condition-camera-fixture"};
    record.non_claims = {
        "no donor runtime code is promoted",
        "no private source path is recorded",
    };
    record.decision =
        "architecture reference only until an independent Cartographer implementation is accepted";
    return record;
}

DonorPromotionReceipt accepted_reimplementation_record() {
    auto record = observed_architecture_record();
    record.promotion_id = "condition-procedural-source-adapter";
    record.target_module = "condition_authoring";
    record.donor_class = DonorClass::promote_implementation;
    record.disposition = PromotionDisposition::reimplemented;
    record.status = PromotionStatus::accepted;
    record.donor.status = DonorEvidenceStatus::privately_verified;
    record.semantic_changes = {
        "bound source identity to the Cartographer project revision",
        "replaced runtime-owned output with a digest-bound authoring record",
    };
    record.removed_authorities = {
        "donor runtime execution is not linked",
        "mutation remains behind the local authoring transaction",
    };
    record.non_claims.clear();
    record.decision =
        "accepted only as an independent public authoring reimplementation";
    return record;
}

DonorPromotionReceipt greenfield_record() {
    DonorPromotionReceipt record;
    record.promotion_id = "cartographer-drafting-tools";
    record.target_module = "drafting_tools";
    record.donor_class = DonorClass::greenfield;
    record.disposition = PromotionDisposition::not_promoted;
    record.status = PromotionStatus::planned;
    record.license_basis = "Cartographer-owned design; no donor source used";
    record.semantic_changes = {
        "define a provider-neutral drafting tool contract",
    };
    record.removed_authorities = {
        "no external runtime or service authority is introduced",
    };
    record.tests = {"planned-adversarial-drafting-fixture"};
    record.fixtures = {"planned-drafting-fixture"};
    record.non_claims = {
        "no donor implementation is claimed",
        "no runtime behavior is claimed",
    };
    record.decision = "planned greenfield work remains outside donor promotion";
    return record;
}

void append_and_round_trip_preserves_lineage() {
    DonorPromotionLedger ledger;
    require(static_cast<bool>(ledger.append(observed_architecture_record())),
        "architecture-only donor evidence appends");
    require(static_cast<bool>(ledger.append(accepted_reimplementation_record())),
        "accepted independent reimplementation appends");
    require(static_cast<bool>(ledger.append(greenfield_record())),
        "greenfield non-claim appends");
    require(ledger.size() == 3U && ledger.records()[0].sequence == 1U &&
            ledger.records()[2].sequence == 3U,
        "ledger assigns one append sequence across donor classes");
    require(ledger.records()[0].receipt_digest != ledger.records()[1].receipt_digest,
        "each receipt is bound to its own canonical content");
    require(static_cast<bool>(ledger.validate()),
        "valid donor promotion history validates");

    const auto serialized = ledger.serialize();
    const auto reopened = DonorPromotionLedger::deserialize(serialized);
    require(reopened && reopened.value().serialize() == serialized,
        "donor promotion history survives deterministic reopen");
    const auto first_digest = ledger.canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "donor promotion ledger identity survives reopen");
    require(reopened.value().records()[0].non_claims.size() == 2U &&
            reopened.value().records()[1].non_claims.empty(),
        "non-claims remain attached to the exact promotion decision");
    require(reopened.value().records()[1].donor.status ==
            DonorEvidenceStatus::privately_verified,
        "private evidence review status survives public receipt reopen");
    require(serialized.find("DONOR_REPOSITORY") == std::string::npos &&
            serialized.find("DONOR_COMMIT") == std::string::npos &&
            serialized.find("REFERENCE ") == std::string::npos,
        "public receipts serialize no donor identity or source reference fields");
}

void receipt_round_trip_and_sequence_independence() {
    auto record = observed_architecture_record();
    record.sequence = 17U;
    require(static_cast<bool>(record.refresh_digest()),
        "a standalone receipt can bind its canonical digest");
    const auto reopened = DonorPromotionReceipt::deserialize(record.serialize());
    require(reopened && reopened.value().sequence == 17U &&
            reopened.value().receipt_digest == record.receipt_digest,
        "standalone receipt round-trips without ledger mutation");
}

void unsafe_or_incomplete_records_fail_closed() {
    {
        auto record = observed_architecture_record();
        record.donor.status = static_cast<DonorEvidenceStatus>(255U);
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "unknown donor evidence state cannot silently fall back");
    }
    {
        auto record = accepted_reimplementation_record();
        record.donor.status = DonorEvidenceStatus::private_review_pending;
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "accepted promotion requires private evidence verification");
    }
    {
        auto record = observed_architecture_record();
        record.decision = "publish https://private.example/token";
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "URLs and token-shaped material are rejected from evidence text");
    }
    {
        const std::array<std::string, 3> object_ids{
            std::string(12U, 'a'), std::string(40U, 'b'), std::string(64U, 'c')};
        for (const auto& object_id : object_ids) {
            auto record = observed_architecture_record();
            record.decision = "reviewed donor object " + object_id;
            DonorPromotionLedger ledger;
            require(!ledger.append(std::move(record)),
                "abbreviated and full object IDs are rejected from evidence text");
        }
    }
    {
        auto record = observed_architecture_record();
        record.semantic_changes = {"reference owner/private-runtime"};
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "repository-shaped references are rejected from evidence text");
    }
    {
        auto record = observed_architecture_record();
        record.promotion_id = std::string(40U, 'd');
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "promotion identifiers cannot be substituted with source object IDs");
    }
    {
        auto record = observed_architecture_record();
        record.sequence = 1U;
        record.decision = "reviewed source object " + std::string(40U, 'e');
        record.receipt_digest = digest_text(record.canonical_without_digest());
        require(!DonorPromotionReceipt::deserialize(record.serialize()),
            "digest-valid persisted receipts cannot smuggle source object IDs");
    }
    {
        auto record = observed_architecture_record();
        record.donor.status = DonorEvidenceStatus::not_applicable;
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "donor-backed records cannot omit their evidence review state");
    }
    {
        auto record = greenfield_record();
        record.donor.status = DonorEvidenceStatus::privately_verified;
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "greenfield work cannot claim donor evidence review");
    }
    {
        auto record = accepted_reimplementation_record();
        record.disposition = PromotionDisposition::not_promoted;
        record.non_claims.clear();
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "an accepted state cannot bypass its implementation disposition");
    }
    {
        auto record = observed_architecture_record();
        record.license_basis.clear();
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "missing licensing justification is rejected");
    }
    {
        auto record = observed_architecture_record();
        record.semantic_changes = {
            "z change",
            "a change",
        };
        DonorPromotionLedger ledger;
        require(!ledger.append(std::move(record)),
            "non-canonical evidence ordering is rejected");
    }
}

void legacy_identity_bearing_schema_fails_closed() {
    const std::string legacy_receipt =
        "CARTOGRAPHER_DONOR_PROMOTION_RECEIPT 1\n"
        "DONOR_REPOSITORY \"private-donor-name\"\n"
        "DONOR_COMMIT \"0123456789abcdef0123456789abcdef01234567\"\n";
    require(!DonorPromotionReceipt::deserialize(legacy_receipt),
        "identity-bearing receipt schema v1 is rejected");

    const std::string legacy_ledger =
        "CARTOGRAPHER_DONOR_PROMOTION_LEDGER 1\nRECORDS 0\nEND\n";
    require(!DonorPromotionLedger::deserialize(legacy_ledger),
        "identity-bearing ledger schema v1 is rejected");
}

void tampering_and_duplicate_mutation_fail_closed() {
    DonorPromotionLedger ledger;
    require(static_cast<bool>(ledger.append(observed_architecture_record())),
        "tamper fixture appends");
    auto duplicate = observed_architecture_record();
    require(!ledger.append(std::move(duplicate)),
        "append-only history rejects duplicate promotion identifiers");

    auto text = ledger.serialize();
    const auto marker = text.find("architecture reference only");
    require(marker != std::string::npos, "tamper fixture contains decision text");
    text.replace(marker, std::string("architecture").size(), "replacement");
    require(!DonorPromotionLedger::deserialize(text),
        "tampered canonical evidence is rejected by its receipt digest");
}

} // namespace

int main() {
    append_and_round_trip_preserves_lineage();
    receipt_round_trip_and_sequence_independence();
    unsafe_or_incomplete_records_fail_closed();
    legacy_identity_bearing_schema_fails_closed();
    tampering_and_duplicate_mutation_fail_closed();
    std::cout << "donor promotion verification tests passed\n";
    return EXIT_SUCCESS;
}
