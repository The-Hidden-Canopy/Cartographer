#include <carto/verification/capability_ledger.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace carto::verification {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_CAPABILITY_LEDGER";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool valid_evidence(production::PreparedCapabilityEvidence evidence) {
    return evidence == production::PreparedCapabilityEvidence::preserved_source ||
        evidence == production::PreparedCapabilityEvidence::derived_product ||
        evidence == production::PreparedCapabilityEvidence::native_realization ||
        evidence == production::PreparedCapabilityEvidence::runtime_validated ||
        evidence == production::PreparedCapabilityEvidence::unsupported;
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

bool read_count(std::istream& input, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) ||
        encoded > CapabilityLedger::kMaxRecords ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
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
            validation("capability ledger digest is invalid"));
    }
    return digest;
}

bool strictly_increasing(const std::vector<std::uint64_t>& values) {
    for (std::size_t index = 1U; index < values.size(); ++index) {
        if (values[index - 1U] >= values[index]) return false;
    }
    return true;
}

core::Result<void> validate_claim_record(const CapabilityClaim& claim) {
    if (claim.sequence == 0U) {
        return core::Result<void>::failure(validation(
            "capability claim sequence is zero"));
    }
    if (!safe_text(claim.feature, CapabilityLedger::kMaxFeatureBytes)) {
        return core::Result<void>::failure(validation(
            "capability claim feature is empty, oversized, or not canonical text"));
    }
    if (!valid_evidence(claim.evidence)) {
        return core::Result<void>::failure(validation(
            "capability claim evidence is not a known evidence class"));
    }
    if (claim.evidence == production::PreparedCapabilityEvidence::unsupported) {
        return core::Result<void>::failure(validation(
            "unsupported capability must be recorded as an explicit non-claim"));
    }
    if (claim.admission_receipt_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "capability claim is not bound to an admission receipt digest"));
    }
    if (!safe_text(claim.detail, CapabilityLedger::kMaxDetailBytes, true)) {
        return core::Result<void>::failure(validation(
            "capability claim detail is oversized or not canonical text"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_non_claim_record(const CapabilityNonClaim& non_claim) {
    if (non_claim.sequence == 0U) {
        return core::Result<void>::failure(validation(
            "capability non-claim sequence is zero"));
    }
    if (!safe_text(non_claim.feature, CapabilityLedger::kMaxFeatureBytes)) {
        return core::Result<void>::failure(validation(
            "capability non-claim feature is empty, oversized, or not canonical text"));
    }
    if (non_claim.admission_receipt_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "capability non-claim is not bound to an admission receipt digest"));
    }
    if (!safe_text(non_claim.reason, CapabilityLedger::kMaxReasonBytes)) {
        return core::Result<void>::failure(validation(
            "capability non-claim reason is empty, oversized, or not canonical text"));
    }
    return core::Result<void>::success();
}

} // namespace

bool CapabilityQueryView::currently_supported() const noexcept {
    if (!latest_claim.has_value()) return false;
    if (!latest_non_claim.has_value()) return true;
    return latest_non_claim->sequence < latest_claim->sequence;
}

core::Result<void> CapabilityClaim::validate() const {
    return validate_claim_record(*this);
}

core::Result<void> CapabilityNonClaim::validate() const {
    return validate_non_claim_record(*this);
}

core::Result<void> CapabilityLedger::claim(CapabilityClaim claim) {
    if (size() >= kMaxRecords) {
        return core::Result<void>::failure(validation(
            "capability ledger has reached its bounded capacity"));
    }
    claim.sequence = next_sequence_++;
    if (auto result = validate_claim_record(claim); !result) {
        --next_sequence_;
        return result;
    }
    claims_.push_back(std::move(claim));
    return core::Result<void>::success();
}

core::Result<void> CapabilityLedger::non_claim(CapabilityNonClaim non_claim) {
    if (size() >= kMaxRecords) {
        return core::Result<void>::failure(validation(
            "capability ledger has reached its bounded capacity"));
    }
    non_claim.sequence = next_sequence_++;
    if (auto result = validate_non_claim_record(non_claim); !result) {
        --next_sequence_;
        return result;
    }
    non_claims_.push_back(std::move(non_claim));
    return core::Result<void>::success();
}

core::Result<void> CapabilityLedger::record_prepared_capabilities(
    const std::vector<production::PreparedCapabilityResult>& capabilities,
    const assets::Sha256Digest& admission_receipt_digest) {
    if (capabilities.size() > kMaxRecords || size() + capabilities.size() > kMaxRecords) {
        return core::Result<void>::failure(validation(
            "capability ledger cannot admit this record batch within its bounds"));
    }
    if (admission_receipt_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "capability records require a non-zero admission receipt digest"));
    }
    for (const auto& capability : capabilities) {
        if (auto result = capability.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("prepared capability record"));
        }
    }
    for (const auto& capability : capabilities) {
        if (capability.evidence == production::PreparedCapabilityEvidence::unsupported) {
            auto result = non_claim(CapabilityNonClaim{
                0U, capability.feature, admission_receipt_digest, capability.detail});
            if (!result) return result;
        } else {
            auto result = claim(CapabilityClaim{
                0U, capability.feature, capability.evidence, capability.required,
                admission_receipt_digest, capability.detail});
            if (!result) return result;
        }
    }
    return core::Result<void>::success();
}

std::optional<CapabilityQueryView> CapabilityLedger::query(
    std::string_view feature) const {
    CapabilityQueryView view;
    view.feature = std::string(feature);
    for (const auto& claim : claims_) {
        if (claim.feature == feature &&
            (!view.latest_claim.has_value() ||
                view.latest_claim->sequence < claim.sequence)) {
            view.latest_claim = claim;
        }
    }
    for (const auto& non_claim : non_claims_) {
        if (non_claim.feature == feature &&
            (!view.latest_non_claim.has_value() ||
                view.latest_non_claim->sequence < non_claim.sequence)) {
            view.latest_non_claim = non_claim;
        }
    }
    if (!view.declared()) return std::nullopt;
    return view;
}

core::Result<void> CapabilityLedger::validate() const {
    if (size() > kMaxRecords) {
        return core::Result<void>::failure(validation(
            "capability ledger exceeds its bounded capacity"));
    }
    std::vector<std::uint64_t> claim_sequences;
    claim_sequences.reserve(claims_.size());
    for (const auto& claim : claims_) {
        if (auto result = validate_claim_record(claim); !result) return result;
        claim_sequences.push_back(claim.sequence);
    }
    if (!strictly_increasing(claim_sequences)) {
        return core::Result<void>::failure(validation(
            "capability claims are not in canonical append order"));
    }
    std::vector<std::uint64_t> non_claim_sequences;
    non_claim_sequences.reserve(non_claims_.size());
    for (const auto& non_claim : non_claims_) {
        if (auto result = validate_non_claim_record(non_claim); !result) return result;
        non_claim_sequences.push_back(non_claim.sequence);
    }
    if (!strictly_increasing(non_claim_sequences)) {
        return core::Result<void>::failure(validation(
            "capability non-claims are not in canonical append order"));
    }
    std::set<std::uint64_t> unique_sequences(
        claim_sequences.begin(), claim_sequences.end());
    unique_sequences.insert(
        non_claim_sequences.begin(), non_claim_sequences.end());
    if (unique_sequences.size() != size()) {
        return core::Result<void>::failure(validation(
            "capability ledger contains a duplicated record sequence"));
    }
    if (!unique_sequences.empty() &&
        *unique_sequences.rbegin() >= next_sequence_) {
        return core::Result<void>::failure(validation(
            "capability ledger sequence cursor is behind its records"));
    }
    return core::Result<void>::success();
}

std::string CapabilityLedger::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "CLAIMS " << claims_.size() << '\n';
    for (const auto& claim : claims_) {
        output << "CLAIM " << claim.sequence << ' ' << std::quoted(claim.feature)
               << ' ' << static_cast<unsigned>(claim.evidence) << ' '
               << (claim.required ? 1U : 0U) << ' '
               << claim.admission_receipt_digest.hex() << ' '
               << std::quoted(claim.detail) << '\n';
    }
    output << "NON_CLAIMS " << non_claims_.size() << '\n';
    for (const auto& non_claim : non_claims_) {
        output << "NON_CLAIM " << non_claim.sequence << ' '
               << std::quoted(non_claim.feature) << ' '
               << non_claim.admission_receipt_digest.hex() << ' '
               << std::quoted(non_claim.reason) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<CapabilityLedger> CapabilityLedger::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<CapabilityLedger>::failure(validation(
            "capability ledger exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kMagic) || !read_unsigned(input, version)) {
        return core::Result<CapabilityLedger>::failure(validation(
            "capability ledger header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<CapabilityLedger>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "capability ledger schema version is unsupported"));
    }

    CapabilityLedger ledger;
    std::size_t count = 0U;
    if (!require_token(input, "CLAIMS") || !read_count(input, count)) {
        return core::Result<CapabilityLedger>::failure(validation(
            "capability claim count is invalid"));
    }
    ledger.claims_.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CapabilityClaim claim;
        std::string encoded_digest;
        unsigned evidence = 0U;
        if (!require_token(input, "CLAIM") ||
            !read_unsigned(input, claim.sequence) ||
            !(input >> std::quoted(claim.feature)) ||
            !read_unsigned(input, evidence) || evidence > 255U ||
            !read_bool(input, claim.required) ||
            !(input >> encoded_digest) ||
            !(input >> std::quoted(claim.detail))) {
            return core::Result<CapabilityLedger>::failure(validation(
                "capability claim record is invalid"));
        }
        claim.evidence = static_cast<production::PreparedCapabilityEvidence>(evidence);
        const auto digest = parse_digest(encoded_digest);
        if (!digest) {
            return core::Result<CapabilityLedger>::failure(
                digest.error().with_context("capability claim receipt"));
        }
        claim.admission_receipt_digest = digest.value();
        if (auto result = validate_claim_record(claim); !result) {
            return core::Result<CapabilityLedger>::failure(result.error());
        }
        ledger.claims_.push_back(std::move(claim));
    }

    if (!require_token(input, "NON_CLAIMS") || !read_count(input, count)) {
        return core::Result<CapabilityLedger>::failure(validation(
            "capability non-claim count is invalid"));
    }
    ledger.non_claims_.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CapabilityNonClaim non_claim;
        std::string encoded_digest;
        if (!require_token(input, "NON_CLAIM") ||
            !read_unsigned(input, non_claim.sequence) ||
            !(input >> std::quoted(non_claim.feature)) ||
            !(input >> encoded_digest) ||
            !(input >> std::quoted(non_claim.reason))) {
            return core::Result<CapabilityLedger>::failure(validation(
                "capability non-claim record is invalid"));
        }
        const auto digest = parse_digest(encoded_digest);
        if (!digest) {
            return core::Result<CapabilityLedger>::failure(
                digest.error().with_context("capability non-claim receipt"));
        }
        non_claim.admission_receipt_digest = digest.value();
        if (auto result = validate_non_claim_record(non_claim); !result) {
            return core::Result<CapabilityLedger>::failure(result.error());
        }
        ledger.non_claims_.push_back(std::move(non_claim));
    }

    if (!require_token(input, "END")) {
        return core::Result<CapabilityLedger>::failure(validation(
            "capability ledger is missing its end marker"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CapabilityLedger>::failure(validation(
            "capability ledger contains trailing data"));
    }
    if (!ledger.claims_.empty()) {
        ledger.next_sequence_ = std::max(
            ledger.next_sequence_, ledger.claims_.back().sequence + 1U);
    }
    if (!ledger.non_claims_.empty()) {
        ledger.next_sequence_ = std::max(
            ledger.next_sequence_, ledger.non_claims_.back().sequence + 1U);
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<CapabilityLedger>::failure(result.error());
    }
    return core::Result<CapabilityLedger>::success(std::move(ledger));
}

core::Result<assets::Sha256Digest> CapabilityLedger::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

} // namespace carto::verification
