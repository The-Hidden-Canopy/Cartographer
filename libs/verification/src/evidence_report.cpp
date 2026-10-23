#include <carto/verification/evidence_report.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::verification {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PIPELINE_EVIDENCE_REPORT";
constexpr std::string_view kEndMarker = "END\n";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_identity(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' ||
            byte == '_' || byte == ':';
    });
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t value_maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > value_maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

core::Result<assets::Sha256Digest> parse_digest(std::string_view encoded) {
    const auto digest = assets::Sha256Digest::from_hex(encoded);
    if (!digest || digest.value().is_zero()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("pipeline evidence report digest is invalid"));
    }
    return digest;
}

bool strictly_increasing(const std::vector<std::uint64_t>& values) {
    for (std::size_t index = 1U; index < values.size(); ++index) {
        if (values[index - 1U] >= values[index]) return false;
    }
    return true;
}

bool correspondence_ordered(
    const CorrespondenceCountExpectation& left,
    const CorrespondenceCountExpectation& right) {
    return std::tuple{
        static_cast<unsigned>(left.source_kind),
        static_cast<unsigned>(left.prepared_kind), left.expected_count} <
        std::tuple{
            static_cast<unsigned>(right.source_kind),
            static_cast<unsigned>(right.prepared_kind), right.expected_count};
}

constexpr std::string_view kGateOutcomePrefix = "GATE_";

} // namespace

core::Result<void> NamedGateOutcome::validate() const {
    if (!safe_identity(gate_identity, kMaxIdentityBytes)) {
        return core::Result<void>::failure(validation(
            "gate identity is empty, oversized, or not a stable identifier"));
    }
    return outcome.validate();
}

core::Result<void> PipelineEvidenceReport::validate() const {
    if (!safe_identity(source_namespace, kMaxIdentityBytes)) {
        return core::Result<void>::failure(validation(
            "evidence report source namespace is not a stable identifier"));
    }
    if (source_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "evidence report requires a non-zero source digest"));
    }
    if (!safe_identity(profile_identity, kMaxIdentityBytes)) {
        return core::Result<void>::failure(validation(
            "evidence report profile identity is not a stable identifier"));
    }
    if (profile_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "evidence report requires a non-zero profile digest"));
    }
    if (capability_claims.size() > CapabilityLedger::kMaxRecords ||
        capability_non_claims.size() > CapabilityLedger::kMaxRecords) {
        return core::Result<void>::failure(validation(
            "evidence report capability records exceed their bounded capacity"));
    }
    std::vector<std::uint64_t> claim_sequences;
    claim_sequences.reserve(capability_claims.size());
    std::set<assets::Sha256Digest> bound_receipts;
    for (const auto& claim : capability_claims) {
        if (auto result = claim.validate(); !result) return result;
        claim_sequences.push_back(claim.sequence);
        bound_receipts.insert(claim.admission_receipt_digest);
    }
    if (!strictly_increasing(claim_sequences)) {
        return core::Result<void>::failure(validation(
            "evidence report capability claims are not in canonical append order"));
    }
    std::vector<std::uint64_t> non_claim_sequences;
    non_claim_sequences.reserve(capability_non_claims.size());
    for (const auto& non_claim : capability_non_claims) {
        if (auto result = non_claim.validate(); !result) return result;
        non_claim_sequences.push_back(non_claim.sequence);
        bound_receipts.insert(non_claim.admission_receipt_digest);
    }
    if (!strictly_increasing(non_claim_sequences)) {
        return core::Result<void>::failure(validation(
            "evidence report capability non-claims are not in canonical append order"));
    }
    std::set<std::uint64_t> unique_sequences(
        claim_sequences.begin(), claim_sequences.end());
    unique_sequences.insert(
        non_claim_sequences.begin(), non_claim_sequences.end());
    if (unique_sequences.size() !=
        capability_claims.size() + capability_non_claims.size()) {
        return core::Result<void>::failure(validation(
            "evidence report capability records reuse a sequence"));
    }
    if (admission_receipt_digests.size() > kMaxReceiptDigests) {
        return core::Result<void>::failure(validation(
            "evidence report receipt digest list exceeds its bounded capacity"));
    }
    for (const auto& receipt : admission_receipt_digests) {
        if (receipt.is_zero()) {
            return core::Result<void>::failure(validation(
                "evidence report admission receipt digest is zero"));
        }
    }
    if (!std::is_sorted(admission_receipt_digests.begin(),
            admission_receipt_digests.end()) ||
        std::adjacent_find(admission_receipt_digests.begin(),
            admission_receipt_digests.end()) != admission_receipt_digests.end()) {
        return core::Result<void>::failure(validation(
            "evidence report admission receipt digests are not unique and sorted"));
    }
    if (bound_receipts != std::set<assets::Sha256Digest>(
            admission_receipt_digests.begin(), admission_receipt_digests.end())) {
        return core::Result<void>::failure(validation(
            "evidence report receipt digest list does not match its capability records"));
    }
    if (correspondence_counts.size() > RegressionGateSpec::kMaxExpectations) {
        return core::Result<void>::failure(validation(
            "evidence report correspondence counts exceed their bounded capacity"));
    }
    for (const auto& correspondence : correspondence_counts) {
        if (auto result = correspondence.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < correspondence_counts.size(); ++index) {
        const auto& previous = correspondence_counts[index - 1U];
        const auto& current = correspondence_counts[index];
        if (previous.source_kind == current.source_kind &&
            previous.prepared_kind == current.prepared_kind) {
            return core::Result<void>::failure(validation(
                "evidence report contains a duplicated correspondence key"));
        }
        if (!correspondence_ordered(previous, current)) {
            return core::Result<void>::failure(validation(
                "evidence report correspondence counts are not in canonical order"));
        }
    }
    if (gate_outcomes.size() > kMaxGateOutcomes) {
        return core::Result<void>::failure(validation(
            "evidence report gate outcomes exceed their bounded capacity"));
    }
    for (const auto& gate : gate_outcomes) {
        if (auto result = gate.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < gate_outcomes.size(); ++index) {
        if (!(gate_outcomes[index - 1U].gate_identity <
                gate_outcomes[index].gate_identity)) {
            return core::Result<void>::failure(validation(
                "evidence report gate outcomes are not in canonical order"));
        }
    }
    if (report_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "evidence report digest is not bound"));
    }
    if (digest_text(canonical_without_digest()) != report_digest) {
        return core::Result<void>::failure(validation(
            "evidence report digest does not match its canonical content"));
    }
    return core::Result<void>::success();
}

std::string PipelineEvidenceReport::canonical_without_digest() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << source_revision.value() << ' ' << source_digest.hex() << '\n';
    output << "PROFILE " << std::quoted(profile_identity) << ' '
           << profile_digest.hex() << '\n';
    output << "CLAIMS " << capability_claims.size() << '\n';
    for (const auto& claim : capability_claims) {
        output << "CLAIM " << claim.sequence << ' ' << std::quoted(claim.feature)
               << ' ' << static_cast<unsigned>(claim.evidence) << ' '
               << (claim.required ? 1U : 0U) << ' '
               << claim.admission_receipt_digest.hex() << ' '
               << std::quoted(claim.detail) << '\n';
    }
    output << "NON_CLAIMS " << capability_non_claims.size() << '\n';
    for (const auto& non_claim : capability_non_claims) {
        output << "NON_CLAIM " << non_claim.sequence << ' '
               << std::quoted(non_claim.feature) << ' '
               << non_claim.admission_receipt_digest.hex() << ' '
               << std::quoted(non_claim.reason) << '\n';
    }
    output << "RECEIPTS " << admission_receipt_digests.size() << '\n';
    for (const auto& receipt : admission_receipt_digests) {
        output << "RECEIPT " << receipt.hex() << '\n';
    }
    output << "CORRESPONDENCES " << correspondence_counts.size() << '\n';
    for (const auto& correspondence : correspondence_counts) {
        output << "CORRESPONDENCE "
               << static_cast<unsigned>(correspondence.source_kind) << ' '
               << static_cast<unsigned>(correspondence.prepared_kind) << ' '
               << correspondence.expected_count << '\n';
    }
    output << "GATES " << gate_outcomes.size() << '\n';
    for (const auto& gate : gate_outcomes) {
        output << "GATE " << std::quoted(gate.gate_identity) << '\n';
        std::istringstream outcome_input(gate.outcome.canonical());
        std::string line;
        while (std::getline(outcome_input, line)) {
            if (!line.empty()) {
                output << kGateOutcomePrefix << line << '\n';
            }
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<void> PipelineEvidenceReport::refresh_digest() {
    report_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string PipelineEvidenceReport::serialize() const {
    std::string text = canonical_without_digest();
    // The digest line binds every canonical byte above it.
    text.replace(text.size() - kEndMarker.size(), kEndMarker.size(),
        "DIGEST " + report_digest.hex() + "\nEND\n");
    return text;
}

core::Result<PipelineEvidenceReport> PipelineEvidenceReport::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kMagic) || !read_unsigned(input, version)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<PipelineEvidenceReport>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "pipeline evidence report schema version is unsupported"));
    }

    PipelineEvidenceReport report;
    std::uint64_t source_revision = 0U;
    std::string encoded_digest;
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(report.source_namespace)) ||
        !read_unsigned(input, source_revision) ||
        !(input >> encoded_digest)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report source identity is invalid"));
    }
    report.source_revision = core::Revision{source_revision};
    const auto source_digest = parse_digest(encoded_digest);
    if (!source_digest) {
        return core::Result<PipelineEvidenceReport>::failure(
            source_digest.error().with_context("pipeline evidence report source"));
    }
    report.source_digest = source_digest.value();
    if (!require_token(input, "PROFILE") ||
        !(input >> std::quoted(report.profile_identity)) ||
        !(input >> encoded_digest)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report profile identity is invalid"));
    }
    const auto profile_digest = parse_digest(encoded_digest);
    if (!profile_digest) {
        return core::Result<PipelineEvidenceReport>::failure(
            profile_digest.error().with_context("pipeline evidence report profile"));
    }
    report.profile_digest = profile_digest.value();

    std::size_t count = 0U;
    if (!require_token(input, "CLAIMS") ||
        !read_count(input, CapabilityLedger::kMaxRecords, count)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report claim count is invalid"));
    }
    report.capability_claims.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CapabilityClaim claim;
        unsigned evidence = 0U;
        unsigned required = 0U;
        if (!require_token(input, "CLAIM") ||
            !read_unsigned(input, claim.sequence) ||
            !(input >> std::quoted(claim.feature)) ||
            !read_unsigned(input, evidence) || evidence > 255U ||
            !read_unsigned(input, required) || required > 1U ||
            !(input >> encoded_digest) ||
            !(input >> std::quoted(claim.detail))) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report claim is invalid"));
        }
        claim.evidence = static_cast<production::PreparedCapabilityEvidence>(evidence);
        claim.required = required != 0U;
        const auto receipt = parse_digest(encoded_digest);
        if (!receipt) {
            return core::Result<PipelineEvidenceReport>::failure(
                receipt.error().with_context("pipeline evidence report claim"));
        }
        claim.admission_receipt_digest = receipt.value();
        report.capability_claims.push_back(std::move(claim));
    }

    if (!require_token(input, "NON_CLAIMS") ||
        !read_count(input, CapabilityLedger::kMaxRecords, count)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report non-claim count is invalid"));
    }
    report.capability_non_claims.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CapabilityNonClaim non_claim;
        if (!require_token(input, "NON_CLAIM") ||
            !read_unsigned(input, non_claim.sequence) ||
            !(input >> std::quoted(non_claim.feature)) ||
            !(input >> encoded_digest) ||
            !(input >> std::quoted(non_claim.reason))) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report non-claim is invalid"));
        }
        const auto receipt = parse_digest(encoded_digest);
        if (!receipt) {
            return core::Result<PipelineEvidenceReport>::failure(
                receipt.error().with_context("pipeline evidence report non-claim"));
        }
        non_claim.admission_receipt_digest = receipt.value();
        report.capability_non_claims.push_back(std::move(non_claim));
    }

    if (!require_token(input, "RECEIPTS") ||
        !read_count(input, kMaxReceiptDigests, count)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report receipt count is invalid"));
    }
    report.admission_receipt_digests.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        if (!require_token(input, "RECEIPT") || !(input >> encoded_digest)) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report receipt is invalid"));
        }
        const auto receipt = parse_digest(encoded_digest);
        if (!receipt) {
            return core::Result<PipelineEvidenceReport>::failure(
                receipt.error().with_context("pipeline evidence report receipt"));
        }
        report.admission_receipt_digests.push_back(receipt.value());
    }

    if (!require_token(input, "CORRESPONDENCES") ||
        !read_count(input, RegressionGateSpec::kMaxExpectations, count)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report correspondence count is invalid"));
    }
    report.correspondence_counts.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CorrespondenceCountExpectation correspondence;
        unsigned source_kind = 0U;
        unsigned prepared_kind = 0U;
        if (!require_token(input, "CORRESPONDENCE") ||
            !read_unsigned(input, source_kind) ||
            !read_unsigned(input, prepared_kind) ||
            !read_unsigned(input, correspondence.expected_count)) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report correspondence is invalid"));
        }
        correspondence.source_kind =
            static_cast<production::PreparedSourceEntityKind>(source_kind);
        correspondence.prepared_kind =
            static_cast<production::PreparedEntityKind>(prepared_kind);
        report.correspondence_counts.push_back(std::move(correspondence));
    }

    if (!require_token(input, "GATES") ||
        !read_count(input, kMaxGateOutcomes, count)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report gate count is invalid"));
    }
    report.gate_outcomes.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        NamedGateOutcome gate;
        if (!require_token(input, "GATE") ||
            !(input >> std::quoted(gate.gate_identity))) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report gate is invalid"));
        }
        std::string remainder;
        if (!std::getline(input, remainder) ||
            !std::all_of(remainder.begin(), remainder.end(),
                [](unsigned char byte) { return byte == ' ' || byte == '\t'; })) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report gate header carries extra data"));
        }
        std::ostringstream outcome_text;
        bool completed = false;
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind(kGateOutcomePrefix, 0U) != 0U) {
                return core::Result<PipelineEvidenceReport>::failure(validation(
                    "pipeline evidence report gate body is malformed"));
            }
            const std::string_view stripped(
                line.begin() + static_cast<std::ptrdiff_t>(kGateOutcomePrefix.size()),
                line.end());
            outcome_text << stripped << '\n';
            if (stripped == "END") {
                completed = true;
                break;
            }
        }
        if (!completed) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence report gate is missing its end marker"));
        }
        const auto outcome = RegressionGateOutcome::deserialize(outcome_text.str());
        if (!outcome) {
            return core::Result<PipelineEvidenceReport>::failure(
                outcome.error().with_context("pipeline evidence report gate"));
        }
        gate.outcome = outcome.value();
        report.gate_outcomes.push_back(std::move(gate));
    }

    if (!require_token(input, "DIGEST") || !(input >> encoded_digest)) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report digest line is missing"));
    }
    const auto report_digest = parse_digest(encoded_digest);
    if (!report_digest) {
        return core::Result<PipelineEvidenceReport>::failure(
            report_digest.error().with_context("pipeline evidence report digest"));
    }
    report.report_digest = report_digest.value();
    if (!require_token(input, "END")) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report is missing its end marker"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report contains trailing data"));
    }
    if (auto result = report.validate(); !result) {
        return core::Result<PipelineEvidenceReport>::failure(result.error());
    }
    return core::Result<PipelineEvidenceReport>::success(std::move(report));
}

core::Result<assets::Sha256Digest> PipelineEvidenceReport::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(report_digest);
}

core::Result<PipelineEvidenceReport> assemble_pipeline_evidence_report(
    const production::PreparedSceneEnvelope& prepared,
    const CapabilityLedger& ledger,
    std::span<const NamedGateOutcome> gate_outcomes) {
    if (auto result = prepared.validate(); !result) {
        return core::Result<PipelineEvidenceReport>::failure(
            result.error().with_context("pipeline evidence prepared scene"));
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<PipelineEvidenceReport>::failure(
            result.error().with_context("pipeline evidence capability ledger"));
    }
    if (gate_outcomes.size() > PipelineEvidenceReport::kMaxGateOutcomes) {
        return core::Result<PipelineEvidenceReport>::failure(validation(
            "pipeline evidence report gate outcome list exceeds its bound"));
    }

    PipelineEvidenceReport report;
    report.source_namespace = prepared.source_namespace;
    report.source_revision = prepared.source_revision;
    report.source_digest = prepared.source_digest;
    report.profile_identity = prepared.profile_identity;
    report.profile_digest = prepared.profile_digest;
    report.capability_claims = ledger.claims();
    report.capability_non_claims = ledger.non_claims();

    std::set<assets::Sha256Digest> receipts;
    for (const auto& claim : report.capability_claims) {
        receipts.insert(claim.admission_receipt_digest);
    }
    for (const auto& non_claim : report.capability_non_claims) {
        receipts.insert(non_claim.admission_receipt_digest);
    }
    report.admission_receipt_digests.assign(receipts.begin(), receipts.end());

    std::map<std::pair<unsigned, unsigned>, std::uint64_t> counts;
    for (const auto& correspondence : prepared.correspondences) {
        const auto key = std::pair{
            static_cast<unsigned>(correspondence.source_kind),
            static_cast<unsigned>(correspondence.prepared_kind)};
        if (counts[key] == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<PipelineEvidenceReport>::failure(validation(
                "pipeline evidence correspondence count exceeds its numeric bound"));
        }
        ++counts[key];
    }
    for (const auto& [key, tally] : counts) {
        report.correspondence_counts.push_back(CorrespondenceCountExpectation{
            static_cast<production::PreparedSourceEntityKind>(key.first),
            static_cast<production::PreparedEntityKind>(key.second),
            tally});
    }

    report.gate_outcomes.assign(gate_outcomes.begin(), gate_outcomes.end());
    std::sort(report.gate_outcomes.begin(), report.gate_outcomes.end(),
        [](const auto& left, const auto& right) {
            return left.gate_identity < right.gate_identity;
        });
    if (auto result = report.refresh_digest(); !result) {
        return core::Result<PipelineEvidenceReport>::failure(result.error());
    }
    if (auto result = report.validate(); !result) {
        return core::Result<PipelineEvidenceReport>::failure(result.error());
    }
    return core::Result<PipelineEvidenceReport>::success(std::move(report));
}

} // namespace carto::verification
