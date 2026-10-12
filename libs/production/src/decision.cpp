#include <carto/production/decision.hpp>

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace carto::production {
namespace {

constexpr std::array<std::uint8_t, 16U> kMagic{
    'C', 'A', 'R', 'T', 'O', '_', 'D', 'E', 'C', 'I', 'S', 'I', 'O', 'N', '1', 0U};
constexpr std::uint32_t kFormatVersion = 1U;
constexpr std::uint32_t kMaxIdentityBytes = 256U;
constexpr std::uint32_t kMaxContextBytes = 128U;
constexpr std::uint32_t kMaxReasonBytes = 128U;
constexpr std::uint32_t kMaxReceiptBytes = 64U * 1024U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool safe_reason(std::string_view value) {
    return !value.empty() && value.size() <= kMaxReasonBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                byte == '.' || byte == ':';
        });
}

bool valid_status(ReceiptStatus status) {
    switch (status) {
    case ReceiptStatus::admitted:
    case ReceiptStatus::failed:
    case ReceiptStatus::stale:
    case ReceiptStatus::cancelled:
        return true;
    }
    return false;
}

bool valid_timing_source(TimingEvidenceSource source) {
    switch (source) {
    case TimingEvidenceSource::none:
    case TimingEvidenceSource::host_clock:
    case TimingEvidenceSource::device_timestamp:
    case TimingEvidenceSource::worker_device_timestamp:
        return true;
    }
    return false;
}

bool valid_disposition(CandidateDisposition disposition) {
    switch (disposition) {
    case CandidateDisposition::selected:
    case CandidateDisposition::rejected_policy:
    case CandidateDisposition::failed:
    case CandidateDisposition::stale:
    case CandidateDisposition::cancelled:
        return true;
    }
    return false;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

core::Result<void> validate_structure(
    const ProductionDecisionReceipt& decision,
    bool check_digest) {
    if (auto result = decision.basis.validate(); !result) return result;
    if (!valid_status(decision.status)) {
        return core::Result<void>::failure(invalid(
            "production decision status is invalid"));
    }
    if (decision.candidates.empty() ||
        decision.candidates.size() > decision.basis.candidate_limit ||
        decision.candidates.size() > ProductionDecisionReceipt::max_candidates) {
        return core::Result<void>::failure(validation(
            "production decision candidate count is empty or exceeds its bound"));
    }

    std::set<std::string> receipt_digests;
    std::set<std::string> cook_keys;
    std::size_t selected_count = 0U;
    bool has_stale = false;
    bool has_cancelled = false;
    for (const auto& candidate : decision.candidates) {
        if (auto result = candidate.validate(decision.basis); !result) return result;
        if (!receipt_digests.insert(candidate.artifact.receipt_digest.hex()).second ||
            !cook_keys.insert(candidate.artifact.key.canonical()).second) {
            return core::Result<void>::failure(validation(
                "production decision contains duplicate candidate evidence"));
        }
        selected_count += candidate.disposition == CandidateDisposition::selected ? 1U : 0U;
        has_stale = has_stale || candidate.disposition == CandidateDisposition::stale;
        has_cancelled = has_cancelled || candidate.disposition == CandidateDisposition::cancelled;
    }

    if (decision.status == ReceiptStatus::admitted) {
        if (!decision.selected_index.has_value() || selected_count != 1U ||
            decision.selected_index.value() >= decision.candidates.size() ||
            decision.candidates[decision.selected_index.value()].disposition !=
                CandidateDisposition::selected) {
            return core::Result<void>::failure(validation(
                "admitted production decisions require exactly one indexed selection"));
        }
    } else if (decision.selected_index.has_value() || selected_count != 0U) {
        return core::Result<void>::failure(validation(
            "non-admitted production decisions may not retain a selected candidate"));
    }
    if (decision.status == ReceiptStatus::stale && !has_stale) {
        return core::Result<void>::failure(validation(
            "stale production decisions require stale candidate evidence"));
    }
    if (decision.status == ReceiptStatus::cancelled && !has_cancelled) {
        return core::Result<void>::failure(validation(
            "cancelled production decisions require cancelled candidate evidence"));
    }
    if (check_digest && (decision.decision_digest.is_zero() ||
        decision.decision_digest != digest_text(decision.canonical_without_digest()))) {
        return core::Result<void>::failure(validation(
            "production decision digest does not match its canonical evidence"));
    }
    return core::Result<void>::success();
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_digest(
    std::vector<std::uint8_t>& output,
    const assets::Sha256Digest& digest) {
    output.insert(output.end(), digest.bytes.begin(), digest.bytes.end());
}

void append_text(std::vector<std::uint8_t>& output, std::string_view value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    bool read_u32(std::uint32_t& value) {
        if (remaining() < sizeof(value)) return false;
        value = 0U;
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            value |= static_cast<std::uint32_t>(bytes_[cursor_++]) << shift;
        }
        return true;
    }

    bool read_u64(std::uint64_t& value) {
        if (remaining() < sizeof(value)) return false;
        value = 0U;
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            value |= static_cast<std::uint64_t>(bytes_[cursor_++]) << shift;
        }
        return true;
    }

    bool read_digest(assets::Sha256Digest& digest) {
        if (remaining() < digest.bytes.size()) return false;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_),
                    digest.bytes.size(), digest.bytes.begin());
        cursor_ += digest.bytes.size();
        return true;
    }

    bool read_text(std::string& value, std::uint32_t maximum, bool allow_empty = false) {
        std::uint32_t size = 0U;
        if (!read_u32(size) || size > maximum || (!allow_empty && size == 0U) ||
            remaining() < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
        cursor_ += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return cursor_ <= bytes_.size() ? bytes_.size() - cursor_ : 0U;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0U;
};

} // namespace

const char* timing_evidence_source_name(TimingEvidenceSource source) noexcept {
    switch (source) {
    case TimingEvidenceSource::none: return "none";
    case TimingEvidenceSource::host_clock: return "host-clock";
    case TimingEvidenceSource::device_timestamp: return "device-timestamp";
    case TimingEvidenceSource::worker_device_timestamp: return "worker-device-timestamp";
    }
    return "unknown";
}

core::Result<void> CostEvidenceBasis::validate(const CostTelemetry& cost) const {
    if (auto result = cost.validate(); !result) return result;
    if (!valid_timing_source(cook) || !valid_timing_source(cpu) ||
        !valid_timing_source(gpu)) {
        return core::Result<void>::failure(invalid(
            "production timing evidence source is invalid"));
    }
    const auto has_basis = [](double value, TimingEvidenceSource source) {
        return (value == 0.0 && source == TimingEvidenceSource::none) ||
            (value > 0.0 && source != TimingEvidenceSource::none);
    };
    if (!has_basis(cost.cook_milliseconds, cook) ||
        !has_basis(cost.cpu_milliseconds, cpu) ||
        !has_basis(cost.gpu_milliseconds, gpu)) {
        return core::Result<void>::failure(validation(
            "production timing values and evidence sources do not agree"));
    }
    if (cost.cook_milliseconds > 0.0 &&
        cook != TimingEvidenceSource::host_clock &&
        cook != TimingEvidenceSource::worker_device_timestamp) {
        return core::Result<void>::failure(validation(
            "production cook time requires host or worker elapsed evidence"));
    }
    if (cost.cpu_milliseconds > 0.0 &&
        cpu != TimingEvidenceSource::host_clock &&
        cpu != TimingEvidenceSource::worker_device_timestamp) {
        return core::Result<void>::failure(validation(
            "production CPU time requires host or worker elapsed evidence"));
    }
    if (cost.gpu_milliseconds > 0.0 &&
        gpu != TimingEvidenceSource::device_timestamp &&
        gpu != TimingEvidenceSource::worker_device_timestamp) {
        return core::Result<void>::failure(validation(
            "production GPU time requires completed device timestamp evidence"));
    }
    return core::Result<void>::success();
}

std::string CostEvidenceBasis::canonical() const {
    return std::string("cook=") + timing_evidence_source_name(cook) +
        ";cpu=" + timing_evidence_source_name(cpu) +
        ";gpu=" + timing_evidence_source_name(gpu);
}

const char* candidate_disposition_name(CandidateDisposition disposition) noexcept {
    switch (disposition) {
    case CandidateDisposition::selected: return "selected";
    case CandidateDisposition::rejected_policy: return "rejected-policy";
    case CandidateDisposition::failed: return "failed";
    case CandidateDisposition::stale: return "stale";
    case CandidateDisposition::cancelled: return "cancelled";
    }
    return "unknown";
}

core::Result<void> ProductionDecisionBasis::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (auto result = context.validate(); !result) return result;
    if (context.truth_class.empty() || context.observer_class.empty() ||
        context.precision_requirement.empty() ||
        context.materialization_requirement.empty()) {
        return core::Result<void>::failure(validation(
            "production decision basis requires complete opaque context identifiers"));
    }
    if (!safe_text(policy_identity, kMaxContextBytes) ||
        admission_policy_digest.is_zero() ||
        !safe_text(hardware_profile, kMaxContextBytes) || candidate_limit == 0U ||
        candidate_limit > ProductionDecisionReceipt::max_candidates) {
        return core::Result<void>::failure(invalid(
            "production decision policy, hardware, or candidate bound is invalid"));
    }
    return core::Result<void>::success();
}

std::string ProductionDecisionBasis::canonical() const {
    std::ostringstream output;
    output << "production-decision-basis-v1{source{" << source.canonical()
           << "};context=" << std::quoted(context.canonical())
           << ";policy=" << std::quoted(policy_identity)
           << ";policy-digest=" << admission_policy_digest.hex()
           << ";hardware=" << std::quoted(hardware_profile)
           << ";candidate-limit=" << candidate_limit << '}';
    return output.str();
}

core::Result<void> ProductionDecisionCandidate::validate(
    const ProductionDecisionBasis& basis) const {
    if (auto result = artifact.validate(); !result) return result;
    if (!valid_disposition(disposition)) {
        return core::Result<void>::failure(invalid(
            "production candidate disposition is invalid"));
    }
    if (artifact.key.source != basis.source ||
        artifact.key.decision_context != basis.context ||
        artifact.key.platform_profile != basis.hardware_profile ||
        artifact.key.representation_kind.empty()) {
        return core::Result<void>::failure(validation(
            "production candidate is not bound to the decision source, context, hardware, and representation"));
    }
    if (auto result = cost_evidence.validate(artifact.cost); !result) return result;

    const bool selected = disposition == CandidateDisposition::selected;
    if ((selected && artifact.status != ReceiptStatus::admitted) ||
        (disposition == CandidateDisposition::rejected_policy &&
         artifact.status != ReceiptStatus::admitted) ||
        (disposition == CandidateDisposition::failed &&
         artifact.status != ReceiptStatus::failed) ||
        (disposition == CandidateDisposition::stale &&
         artifact.status != ReceiptStatus::stale) ||
        (disposition == CandidateDisposition::cancelled &&
         artifact.status != ReceiptStatus::cancelled)) {
        return core::Result<void>::failure(validation(
            "production candidate disposition does not match artifact status"));
    }
    if ((selected && !reason_code.empty()) ||
        (!selected && !safe_reason(reason_code))) {
        return core::Result<void>::failure(invalid(
            "production candidate rejection reason is missing or unsafe"));
    }
    return core::Result<void>::success();
}

std::string ProductionDecisionCandidate::canonical() const {
    std::ostringstream output;
    output << "candidate{disposition=" << candidate_disposition_name(disposition)
           << ";reason=" << std::quoted(reason_code)
           << ";cost-evidence{" << cost_evidence.canonical() << "}"
           << ";artifact{" << artifact.canonical_without_digest()
           << ";receipt-digest=" << artifact.receipt_digest.hex() << "}}";
    return output.str();
}

core::Result<void> ProductionDecisionReceipt::validate() const {
    return validate_structure(*this, true);
}

std::string ProductionDecisionReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << "production-decision-v1{" << basis.canonical()
           << ";status=" << receipt_status_name(status) << ";selected=";
    if (selected_index.has_value()) {
        output << selected_index.value();
    } else {
        output << "none";
    }
    output << ";candidates=" << candidates.size();
    for (std::size_t index = 0U; index < candidates.size(); ++index) {
        output << ";candidate[" << index << "]=" << candidates[index].canonical();
    }
    output << '}';
    return output.str();
}

core::Result<void> ProductionDecisionReceipt::refresh_digest() {
    if (auto result = validate_structure(*this, false); !result) return result;
    decision_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

const ProductionDecisionCandidate*
ProductionDecisionReceipt::selected_candidate() const noexcept {
    if (!selected_index.has_value() || selected_index.value() >= candidates.size()) {
        return nullptr;
    }
    return &candidates[selected_index.value()];
}

core::Result<void> validate_decision_publication(
    const ProductionDecisionReceipt& decision,
    const AuthoritativeSource& current_source,
    const AdmissionPolicy& current_policy) {
    if (auto result = decision.validate(); !result) return result;
    if (auto result = current_source.validate(); !result) return result;
    const auto policy_digest = current_policy.canonical_digest();
    if (!policy_digest) return core::Result<void>::failure(policy_digest.error());
    if (decision.basis.source != current_source) {
        return core::Result<void>::failure(stale(
            "production decision source revision or digest is stale"));
    }
    if (decision.basis.admission_policy_digest != policy_digest.value()) {
        return core::Result<void>::failure(stale(
            "production decision admission policy is stale"));
    }
    if (decision.status != ReceiptStatus::admitted ||
        decision.selected_candidate() == nullptr) {
        return core::Result<void>::failure(validation(
            "only admitted production decisions may publish a selected artifact"));
    }
    return validate_publication(
        decision.selected_candidate()->artifact, current_source, current_policy);
}

core::Result<std::vector<std::uint8_t>> serialize_production_decision(
    const ProductionDecisionReceipt& decision) {
    if (auto result = decision.validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }

    std::vector<std::uint8_t> output;
    output.reserve(1024U + decision.candidates.size() * 2048U);
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    append_u32(output, kFormatVersion);
    append_text(output, decision.basis.source.identity);
    append_u64(output, decision.basis.source.revision.value());
    append_digest(output, decision.basis.source.content_digest);
    append_text(output, decision.basis.context.truth_class);
    append_text(output, decision.basis.context.observer_class);
    append_text(output, decision.basis.context.precision_requirement);
    append_text(output, decision.basis.context.materialization_requirement);
    append_text(output, decision.basis.policy_identity);
    append_digest(output, decision.basis.admission_policy_digest);
    append_text(output, decision.basis.hardware_profile);
    append_u32(output, decision.basis.candidate_limit);
    append_u32(output, static_cast<std::uint32_t>(decision.status));
    append_u32(output, decision.selected_index.value_or(
        std::numeric_limits<std::uint32_t>::max()));
    append_u32(output, static_cast<std::uint32_t>(decision.candidates.size()));

    for (const auto& candidate : decision.candidates) {
        append_u32(output, static_cast<std::uint32_t>(candidate.disposition));
        append_u32(output, static_cast<std::uint32_t>(candidate.cost_evidence.cook));
        append_u32(output, static_cast<std::uint32_t>(candidate.cost_evidence.cpu));
        append_u32(output, static_cast<std::uint32_t>(candidate.cost_evidence.gpu));
        append_text(output, candidate.reason_code);

        ProductionReceiptLedger ledger;
        if (auto result = ledger.append(candidate.artifact); !result) {
            return core::Result<std::vector<std::uint8_t>>::failure(result.error());
        }
        append_text(output, ledger.serialize());
    }
    append_digest(output, decision.decision_digest);
    if (output.size() > ProductionDecisionReceipt::max_serialized_bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "serialized production decision exceeds its byte bound"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(output));
}

core::Result<ProductionDecisionReceipt> deserialize_production_decision(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() > ProductionDecisionReceipt::max_serialized_bytes ||
        bytes.size() < kMagic.size() + sizeof(std::uint32_t)) {
        return core::Result<ProductionDecisionReceipt>::failure(validation(
            "serialized production decision is empty, truncated, or oversized"));
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return core::Result<ProductionDecisionReceipt>::failure(validation(
            "serialized production decision has unknown framing"));
    }

    Reader reader(bytes.subspan(kMagic.size()));
    std::uint32_t version = 0U;
    ProductionDecisionReceipt decision;
    std::uint64_t revision = 0U;
    std::uint32_t status = 0U;
    std::uint32_t selected_index = 0U;
    std::uint32_t candidate_count = 0U;
    if (!reader.read_u32(version) || version != kFormatVersion ||
        !reader.read_text(decision.basis.source.identity, kMaxIdentityBytes) ||
        !reader.read_u64(revision) ||
        !reader.read_digest(decision.basis.source.content_digest) ||
        !reader.read_text(decision.basis.context.truth_class, kMaxContextBytes) ||
        !reader.read_text(decision.basis.context.observer_class, kMaxContextBytes) ||
        !reader.read_text(decision.basis.context.precision_requirement, kMaxContextBytes) ||
        !reader.read_text(
            decision.basis.context.materialization_requirement, kMaxContextBytes) ||
        !reader.read_text(decision.basis.policy_identity, kMaxContextBytes) ||
        !reader.read_digest(decision.basis.admission_policy_digest) ||
        !reader.read_text(decision.basis.hardware_profile, kMaxContextBytes) ||
        !reader.read_u32(decision.basis.candidate_limit) ||
        !reader.read_u32(status) || !reader.read_u32(selected_index) ||
        !reader.read_u32(candidate_count) || candidate_count == 0U ||
        candidate_count > ProductionDecisionReceipt::max_candidates ||
        candidate_count > decision.basis.candidate_limit) {
        return core::Result<ProductionDecisionReceipt>::failure(validation(
            "serialized production decision header is malformed"));
    }
    decision.basis.source.revision = core::Revision{revision};
    decision.status = static_cast<ReceiptStatus>(status);
    if (selected_index != std::numeric_limits<std::uint32_t>::max()) {
        decision.selected_index = selected_index;
    }
    decision.candidates.reserve(candidate_count);

    for (std::uint32_t index = 0U; index < candidate_count; ++index) {
        ProductionDecisionCandidate candidate;
        std::uint32_t disposition = 0U;
        std::uint32_t cook = 0U;
        std::uint32_t cpu = 0U;
        std::uint32_t gpu = 0U;
        std::string serialized_receipt;
        if (!reader.read_u32(disposition) || !reader.read_u32(cook) ||
            !reader.read_u32(cpu) || !reader.read_u32(gpu) ||
            !reader.read_text(candidate.reason_code, kMaxReasonBytes, true) ||
            !reader.read_text(serialized_receipt, kMaxReceiptBytes)) {
            return core::Result<ProductionDecisionReceipt>::failure(validation(
                "serialized production decision candidate is malformed"));
        }
        candidate.disposition = static_cast<CandidateDisposition>(disposition);
        candidate.cost_evidence = {
            static_cast<TimingEvidenceSource>(cook),
            static_cast<TimingEvidenceSource>(cpu),
            static_cast<TimingEvidenceSource>(gpu)};
        const auto ledger = ProductionReceiptLedger::deserialize(serialized_receipt);
        if (!ledger || ledger.value().records().size() != 1U) {
            return core::Result<ProductionDecisionReceipt>::failure(validation(
                "serialized production decision does not contain exactly one receipt per candidate"));
        }
        candidate.artifact = ledger.value().records().front();
        decision.candidates.push_back(std::move(candidate));
    }

    if (!reader.read_digest(decision.decision_digest) || reader.remaining() != 0U) {
        return core::Result<ProductionDecisionReceipt>::failure(validation(
            "serialized production decision digest is missing or has trailing data"));
    }
    if (auto result = decision.validate(); !result) {
        return core::Result<ProductionDecisionReceipt>::failure(result.error());
    }
    return core::Result<ProductionDecisionReceipt>::success(std::move(decision));
}

} // namespace carto::production
