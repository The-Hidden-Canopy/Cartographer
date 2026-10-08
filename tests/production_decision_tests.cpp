#include <carto/production/decision.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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

carto::production::AdmissionPolicy policy_fixture() {
    return {
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 32'768U,
        .max_cook_milliseconds = 8.0,
        .max_cpu_milliseconds = 2.0,
        .max_gpu_milliseconds = 2.0,
        .max_disk_bytes = 16'384U,
        .max_output_bytes = 16'384U,
    };
}

carto::production::ProductionDecisionBasis basis_fixture() {
    const auto policy_digest = policy_fixture().canonical_digest();
    require(static_cast<bool>(policy_digest), "decision policy fixture has a digest");
    return {
        .source = {
            "project/public-fixture/area-01",
            carto::core::Revision{12U},
            digest("public-area-v12")},
        .context = {
            .truth_class = "derived",
            .observer_class = "lookdev-viewport",
            .precision_requirement = "native-pbr-f32",
            .materialization_requirement = "resident-visible",
        },
        .policy_identity = "public-threshold-gate-v1",
        .admission_policy_digest = policy_digest.value(),
        .hardware_profile = "d3d12-test-device",
        .candidate_limit = 4U,
    };
}

carto::production::ProductionArtifactReceipt admitted_artifact(
    const carto::production::ProductionDecisionBasis& basis,
    std::string_view representation,
    std::string_view algorithm,
    double error,
    double gpu_milliseconds) {
    using namespace carto::production;
    ProductionArtifactReceipt receipt;
    receipt.key.source = basis.source;
    receipt.key.kind = ArtifactKind::publication_package;
    receipt.key.form = ExecutionForm::interactive;
    receipt.key.algorithm = std::string(algorithm);
    receipt.key.settings_digest = digest(std::string(algorithm) + "-settings");
    receipt.key.platform_profile = basis.hardware_profile;
    receipt.key.representation_kind = std::string(representation);
    receipt.key.spatial_scope = "public-grid/area-01/hero-surface";
    receipt.key.decision_context = basis.context;
    receipt.status = ReceiptStatus::admitted;
    receipt.output_digest = digest(std::string(representation) + "-output");
    receipt.output_bytes = 4096U;
    receipt.quality = {
        .measured_error = error,
        .importance = 0.95,
        .confidence = 0.98,
    };
    receipt.cost = {
        .cook_milliseconds = 4.0,
        .cpu_milliseconds = 0.5,
        .gpu_milliseconds = gpu_milliseconds,
        .resident_bytes = 8192U,
        .disk_bytes = 4096U,
    };
    require(static_cast<bool>(receipt.refresh_digest()),
            "decision artifact fixture refreshes its receipt digest");
    return receipt;
}

carto::production::ProductionDecisionCandidate candidate(
    carto::production::ProductionArtifactReceipt artifact,
    carto::production::CandidateDisposition disposition,
    std::string reason) {
    using namespace carto::production;
    return {
        .artifact = std::move(artifact),
        .disposition = disposition,
        .cost_evidence = {
            .cook = TimingEvidenceSource::host_clock,
            .cpu = TimingEvidenceSource::host_clock,
            .gpu = TimingEvidenceSource::device_timestamp,
        },
        .reason_code = std::move(reason),
    };
}

carto::production::ProductionDecisionReceipt decision_fixture() {
    using namespace carto::production;
    const auto basis = basis_fixture();
    ProductionDecisionReceipt decision;
    decision.basis = basis;
    decision.status = ReceiptStatus::admitted;
    decision.selected_index = 1U;
    decision.candidates.push_back(candidate(
        admitted_artifact(basis, "fast-preview", "preview-cook-v1", 0.08, 0.4),
        CandidateDisposition::rejected_policy,
        "measured-error-budget"));
    decision.candidates.push_back(candidate(
        admitted_artifact(basis, "native-lookdev", "native-cook-v1", 0.01, 1.2),
        CandidateDisposition::selected,
        {}));
    require(static_cast<bool>(decision.refresh_digest()),
            "production decision fixture refreshes its digest");
    return decision;
}

void decision_round_trips_and_publishes() {
    using namespace carto::production;
    const auto decision = decision_fixture();
    require(static_cast<bool>(decision.validate()), "production decision validates");
    require(decision.selected_candidate() != nullptr &&
                decision.selected_candidate()->artifact.key.representation_kind ==
                    "native-lookdev",
            "production decision exposes the exact selected candidate");
    require(static_cast<bool>(validate_decision_publication(
                decision, decision.basis.source, policy_fixture())),
            "source-current threshold-bound decision publishes");

    const auto bytes = serialize_production_decision(decision);
    require(static_cast<bool>(bytes), "production decision serializes");
    const auto reopened = deserialize_production_decision(bytes.value());
    require(reopened && reopened.value().decision_digest == decision.decision_digest &&
                reopened.value().canonical_without_digest() ==
                    decision.canonical_without_digest(),
            "production decision survives deterministic save and reopen");
}

void policy_digest_is_semantic_and_deterministic() {
    auto positive_zero = policy_fixture();
    positive_zero.max_measured_error = 0.0;
    positive_zero.min_confidence = 0.0;
    auto negative_zero = positive_zero;
    negative_zero.max_measured_error = -0.0;
    negative_zero.min_confidence = -0.0;
    const auto first = positive_zero.canonical_digest();
    const auto second = negative_zero.canonical_digest();
    require(first && second && first.value() == second.value(),
            "semantically equal signed-zero policies share one canonical digest");
}

void stale_policy_source_and_output_fail_closed() {
    using namespace carto::production;
    auto decision = decision_fixture();
    auto stale_source = decision.basis.source;
    stale_source.revision = carto::core::Revision{13U};
    const auto stale_result = validate_decision_publication(
        decision, stale_source, policy_fixture());
    require(!stale_result &&
                stale_result.error().code == carto::core::ErrorCode::stale_data,
            "edited authoritative source invalidates the prior decision");

    auto changed_policy = policy_fixture();
    changed_policy.max_gpu_milliseconds = 1.0;
    const auto changed_policy_result = validate_decision_publication(
        decision, decision.basis.source, changed_policy);
    require(!changed_policy_result &&
                changed_policy_result.error().code ==
                    carto::core::ErrorCode::stale_data,
            "changed admission policy invalidates the bound decision");

    decision.candidates[1U].artifact.quality.measured_error = 0.2;
    require(static_cast<bool>(decision.candidates[1U].artifact.refresh_digest()),
            "over-budget selected artifact remains structurally valid evidence");
    require(static_cast<bool>(decision.refresh_digest()),
            "decision rebinds the changed selected receipt");
    require(!validate_decision_publication(
                decision, decision.basis.source, policy_fixture()),
            "selected artifact outside current quality policy cannot publish");
}

void malformed_decisions_and_timing_claims_fail_closed() {
    using namespace carto::production;
    auto decision = decision_fixture();
    decision.candidates[1U].cost_evidence.gpu = TimingEvidenceSource::host_clock;
    require(!decision.refresh_digest(),
            "host timing cannot be relabeled as measured GPU time");

    decision = decision_fixture();
    decision.candidates[1U].reason_code = "selected-has-no-rejection";
    require(!decision.refresh_digest(),
            "selected candidate cannot carry a rejection reason");

    decision = decision_fixture();
    decision.candidates[0U].reason_code.clear();
    require(!decision.refresh_digest(),
            "rejected candidate requires a bounded reason code");

    decision = decision_fixture();
    decision.candidates.push_back(decision.candidates[0U]);
    require(!decision.refresh_digest(),
            "duplicate candidate evidence is rejected");

    decision = decision_fixture();
    decision.candidates[0U].disposition = CandidateDisposition::selected;
    decision.candidates[0U].reason_code.clear();
    require(!decision.refresh_digest(),
            "two selected candidates are rejected");

    decision = decision_fixture();
    decision.candidates[1U].artifact.key.decision_context.observer_class = "sensor";
    require(static_cast<bool>(decision.candidates[1U].artifact.refresh_digest()),
            "context substitution can produce internally valid artifact evidence");
    require(!decision.refresh_digest(),
            "candidate context substitution cannot enter the decision");

    decision = decision_fixture();
    decision.basis.candidate_limit = 1U;
    require(!decision.refresh_digest(),
            "candidate population cannot exceed its declared finite bound");

    decision = decision_fixture();
    decision.status = static_cast<ReceiptStatus>(255U);
    require(!decision.refresh_digest(), "unknown decision states fail closed");

    decision = decision_fixture();
    decision.candidates[0U].disposition =
        static_cast<CandidateDisposition>(255U);
    require(!decision.refresh_digest(),
            "unknown candidate dispositions fail closed");

    decision = decision_fixture();
    decision.candidates[1U].cost_evidence.gpu =
        static_cast<TimingEvidenceSource>(255U);
    require(!decision.refresh_digest(),
            "unknown timing evidence sources fail closed");
}

void serialization_rejects_tamper_and_trailing_data() {
    using namespace carto::production;
    const auto bytes = serialize_production_decision(decision_fixture());
    require(static_cast<bool>(bytes), "tamper fixture serializes");

    auto wrong_magic = bytes.value();
    wrong_magic.front() = 'X';
    require(!deserialize_production_decision(wrong_magic),
            "unknown decision framing is rejected");

    auto truncated = bytes.value();
    truncated.pop_back();
    require(!deserialize_production_decision(truncated),
            "truncated decision digest is rejected");

    auto trailing = bytes.value();
    trailing.push_back(0U);
    require(!deserialize_production_decision(trailing),
            "trailing unbound decision bytes are rejected");

    auto tampered = bytes.value();
    tampered[tampered.size() / 2U] ^= 1U;
    require(!deserialize_production_decision(tampered),
            "tampered nested receipt or decision evidence is rejected");
}

void non_admitted_decision_cannot_publish() {
    using namespace carto::production;
    const auto basis = basis_fixture();
    ProductionArtifactReceipt failed;
    failed.key.source = basis.source;
    failed.key.kind = ArtifactKind::publication_package;
    failed.key.form = ExecutionForm::offline;
    failed.key.algorithm = "failed-cook-v1";
    failed.key.settings_digest = digest("failed-settings");
    failed.key.platform_profile = basis.hardware_profile;
    failed.key.representation_kind = "reference";
    failed.key.spatial_scope = "public-grid/area-01/hero-surface";
    failed.key.decision_context = basis.context;
    failed.status = ReceiptStatus::failed;
    require(static_cast<bool>(failed.refresh_digest()),
            "failed candidate remains valid no-output evidence");

    ProductionDecisionReceipt decision;
    decision.basis = basis;
    decision.status = ReceiptStatus::failed;
    decision.candidates.push_back({
        .artifact = std::move(failed),
        .disposition = CandidateDisposition::failed,
        .cost_evidence = {},
        .reason_code = "unsupported-input",
    });
    require(static_cast<bool>(decision.refresh_digest()),
            "failed decision is durable evidence");
    require(!validate_decision_publication(
                decision, decision.basis.source, policy_fixture()),
            "failed decision cannot publish a fallback artifact");
}

} // namespace

int main() {
    decision_round_trips_and_publishes();
    policy_digest_is_semantic_and_deterministic();
    stale_policy_source_and_output_fail_closed();
    malformed_decisions_and_timing_claims_fail_closed();
    serialization_rejects_tamper_and_trailing_data();
    non_admitted_decision_cannot_publish();
    std::cout << "production decision tests passed\n";
    return EXIT_SUCCESS;
}
