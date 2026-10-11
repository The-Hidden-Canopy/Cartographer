#include <carto/collision/benchmark.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <system_error>
#include <utility>

namespace carto::collision {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_COLLISION_BENCHMARK_RECEIPT";
constexpr std::size_t kMaxTextBytes = 256U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool valid_status(CollisionReceiptStatus status) {
    switch (status) {
    case CollisionReceiptStatus::admitted:
    case CollisionReceiptStatus::failed:
    case CollisionReceiptStatus::stale:
    case CollisionReceiptStatus::cancelled:
        return true;
    }
    return false;
}

bool valid_evidence(CollisionBenchmarkEvidence evidence) {
    switch (evidence) {
    case CollisionBenchmarkEvidence::structural_validation:
    case CollisionBenchmarkEvidence::cpu_reference:
    case CollisionBenchmarkEvidence::native_backend_evidence:
    case CollisionBenchmarkEvidence::full_game_acceptance:
        return true;
    }
    return false;
}

CollisionReceiptStatus parse_status(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(CollisionReceiptStatus::cancelled);
         ++value) {
        const auto status = static_cast<CollisionReceiptStatus>(value);
        if (collision_receipt_status_name(status) == text) return status;
    }
    return static_cast<CollisionReceiptStatus>(0xFFU);
}

CollisionBenchmarkEvidence parse_evidence(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(
             CollisionBenchmarkEvidence::full_game_acceptance);
         ++value) {
        const auto evidence = static_cast<CollisionBenchmarkEvidence>(value);
        if (collision_benchmark_evidence_name(evidence) == text) return evidence;
    }
    return static_cast<CollisionBenchmarkEvidence>(0xFFU);
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

bool read_metrics(std::istream& input, CollisionErrorMetrics& metrics) {
    return require_token(input, "METRICS") &&
        require_token(input, "VOLUME_OVERLAP") &&
        read_double(input, metrics.volume_overlap_residual) &&
        require_token(input, "PENETRATION") &&
        read_double(input, metrics.max_penetration_depth) &&
        require_token(input, "HAUSDORFF") &&
        read_double(input, metrics.one_sided_hausdorff) &&
        require_token(input, "CLEARANCE") &&
        read_double(input, metrics.clearance_violation);
}

} // namespace

std::string_view collision_benchmark_evidence_name(
    CollisionBenchmarkEvidence evidence) noexcept {
    switch (evidence) {
    case CollisionBenchmarkEvidence::structural_validation:
        return "structural_validation";
    case CollisionBenchmarkEvidence::cpu_reference:
        return "cpu_reference";
    case CollisionBenchmarkEvidence::native_backend_evidence:
        return "native_backend_evidence";
    case CollisionBenchmarkEvidence::full_game_acceptance:
        return "full_game_acceptance";
    }
    return "unknown";
}

core::Result<void> CollisionBenchmarkBasis::validate() const {
    if (!safe_text(source_identity, kMaxTextBytes) ||
        !safe_text(algorithm, kMaxTextBytes) ||
        !safe_text(target_profile_identity, kMaxTextBytes) ||
        !safe_text(hardware_profile, kMaxTextBytes)) {
        return core::Result<void>::failure(invalid(
            "collision benchmark basis text fields are invalid"));
    }
    if (source_revision.value() == 0U) {
        return core::Result<void>::failure(validation(
            "collision benchmark basis must bind a committed source revision"));
    }
    if (source_digest.is_zero() || settings_digest.is_zero() ||
        target_profile_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "collision benchmark basis must bind every evidence digest"));
    }
    return core::Result<void>::success();
}

std::string CollisionBenchmarkBasis::canonical() const {
    std::ostringstream output;
    output << "SOURCE " << std::quoted(source_identity)
           << " REVISION " << source_revision.value()
           << " SOURCE_DIGEST " << source_digest.hex()
           << " ALGORITHM " << std::quoted(algorithm)
           << " SETTINGS_DIGEST " << settings_digest.hex()
           << " PROFILE " << std::quoted(target_profile_identity)
           << " PROFILE_DIGEST " << target_profile_digest.hex()
           << " HARDWARE " << std::quoted(hardware_profile);
    return output.str();
}

core::Result<void> CollisionBenchmarkReceipt::validate() const {
    if (auto result = basis.validate(); !result) return result;
    if (candidate_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "collision benchmark receipt must bind its candidate digest"));
    }
    if (auto result = measured_error.validate(); !result) return result;
    if (!std::isfinite(cpu_reference_milliseconds) ||
        cpu_reference_milliseconds < 0.0) {
        return core::Result<void>::failure(validation(
            "collision benchmark CPU reference time must be finite and "
            "non-negative host-clock evidence"));
    }
    if (!valid_evidence(evidence)) {
        return core::Result<void>::failure(invalid(
            "collision benchmark evidence tier is invalid"));
    }
    // Evidence tiers never promote silently: neutral preparation cannot
    // fabricate native backend or full-game acceptance evidence.
    if (evidence == CollisionBenchmarkEvidence::native_backend_evidence ||
        evidence == CollisionBenchmarkEvidence::full_game_acceptance) {
        return core::Result<void>::failure(validation(
            "collision benchmark receipt claims evidence neutral preparation "
            "cannot produce"));
    }
    if (!valid_status(status)) {
        return core::Result<void>::failure(invalid(
            "collision benchmark receipt status is invalid"));
    }
    if (status == CollisionReceiptStatus::admitted &&
        evidence == CollisionBenchmarkEvidence::structural_validation) {
        return core::Result<void>::failure(validation(
            "structural validation alone never admits a collision candidate"));
    }
    if (receipt_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "collision benchmark receipt must be digest-bound via refresh_digest"));
    }
    return core::Result<void>::success();
}

std::string CollisionBenchmarkReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "BASIS " << basis.canonical() << '\n';
    output << std::setprecision(17)
           << "CANDIDATE_DIGEST " << candidate_digest.hex()
           << ' ' << measured_error.canonical()
           << " CPU_MS " << cpu_reference_milliseconds
           << " EVIDENCE " << collision_benchmark_evidence_name(evidence)
           << " STATUS " << collision_receipt_status_name(status) << '\n';
    return output.str();
}

std::string CollisionBenchmarkReceipt::canonical() const {
    return canonical_without_digest() + "DIGEST " + receipt_digest.hex() + "\n";
}

core::Result<void> CollisionBenchmarkReceipt::refresh_digest() {
    receipt_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string CollisionBenchmarkReceipt::serialize() const {
    return canonical() + "END\n";
}

core::Result<CollisionBenchmarkReceipt> CollisionBenchmarkReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CollisionBenchmarkReceipt>::failure(parse_error(
            "collision benchmark receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic || !read_integer(input, version) ||
        version != kSchemaVersion) {
        return core::Result<CollisionBenchmarkReceipt>::failure(parse_error(
            "collision benchmark receipt framing or schema version is invalid"));
    }
    CollisionBenchmarkReceipt receipt;
    std::uint64_t source_revision = 0U;
    if (!require_token(input, "BASIS") ||
        !require_token(input, "SOURCE") ||
        !(input >> std::quoted(receipt.basis.source_identity)) ||
        !require_token(input, "REVISION") ||
        !read_integer(input, source_revision) ||
        !require_token(input, "SOURCE_DIGEST") ||
        !read_digest(input, receipt.basis.source_digest) ||
        !require_token(input, "ALGORITHM") ||
        !(input >> std::quoted(receipt.basis.algorithm)) ||
        !require_token(input, "SETTINGS_DIGEST") ||
        !read_digest(input, receipt.basis.settings_digest) ||
        !require_token(input, "PROFILE") ||
        !(input >> std::quoted(receipt.basis.target_profile_identity)) ||
        !require_token(input, "PROFILE_DIGEST") ||
        !read_digest(input, receipt.basis.target_profile_digest) ||
        !require_token(input, "HARDWARE") ||
        !(input >> std::quoted(receipt.basis.hardware_profile)) ||
        !require_token(input, "CANDIDATE_DIGEST") ||
        !read_digest(input, receipt.candidate_digest) ||
        !read_metrics(input, receipt.measured_error) ||
        !require_token(input, "CPU_MS") ||
        !read_double(input, receipt.cpu_reference_milliseconds) ||
        !require_token(input, "EVIDENCE")) {
        return core::Result<CollisionBenchmarkReceipt>::failure(parse_error(
            "collision benchmark receipt body is malformed"));
    }
    receipt.basis.source_revision = core::Revision{source_revision};
    std::string evidence;
    std::string status;
    if (!(input >> evidence) ||
        !require_token(input, "STATUS") || !(input >> status) ||
        !require_token(input, "DIGEST") ||
        !read_digest(input, receipt.receipt_digest) ||
        !require_token(input, "END")) {
        return core::Result<CollisionBenchmarkReceipt>::failure(parse_error(
            "collision benchmark receipt trailer is malformed"));
    }
    receipt.evidence = parse_evidence(evidence);
    receipt.status = parse_status(status);
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CollisionBenchmarkReceipt>::failure(parse_error(
            "collision benchmark receipt has trailing data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<CollisionBenchmarkReceipt>::failure(result.error());
    }
    const assets::Sha256Digest expected =
        digest_text(receipt.canonical_without_digest());
    if (expected != receipt.receipt_digest) {
        return core::Result<CollisionBenchmarkReceipt>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "collision benchmark receipt digest does not match its content"));
    }
    return core::Result<CollisionBenchmarkReceipt>::success(std::move(receipt));
}

core::Result<void> CollisionPromotionGate::validate() const {
    if (auto result = error_thresholds.validate(); !result) return result;
    if (max_cpu_reference_milliseconds.has_value() &&
        (!std::isfinite(max_cpu_reference_milliseconds.value()) ||
            max_cpu_reference_milliseconds.value() < 0.0)) {
        return core::Result<void>::failure(validation(
            "collision promotion gate CPU bound must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

std::string CollisionPromotionGate::canonical() const {
    std::ostringstream output;
    output << error_thresholds.canonical() << " CPU_MS ";
    if (max_cpu_reference_milliseconds.has_value()) {
        output << std::setprecision(17) << max_cpu_reference_milliseconds.value();
    } else {
        output << '-';
    }
    return output.str();
}

core::Result<CollisionPromotionDecision> promote_collision_candidate(
    const CollisionBenchmarkReceipt& candidate,
    const std::optional<assets::Sha256Digest>& last_good_candidate_digest,
    const CollisionPromotionGate& gate) {
    if (auto result = gate.validate(); !result) {
        return core::Result<CollisionPromotionDecision>::failure(result.error());
    }
    if (auto result = candidate.validate(); !result) {
        return core::Result<CollisionPromotionDecision>::failure(result.error());
    }

    const bool admitted =
        candidate.status == CollisionReceiptStatus::admitted &&
        static_cast<bool>(evaluate_collision_error(
            candidate.measured_error, gate.error_thresholds)) &&
        (!gate.max_cpu_reference_milliseconds.has_value() ||
            candidate.cpu_reference_milliseconds <=
                gate.max_cpu_reference_milliseconds.value());
    if (admitted) {
        CollisionPromotionDecision decision;
        decision.selected_candidate_digest = candidate.candidate_digest;
        decision.promoted_new_candidate = true;
        decision.retained_last_good = false;
        return core::Result<CollisionPromotionDecision>::success(decision);
    }
    if (!last_good_candidate_digest.has_value()) {
        return core::Result<CollisionPromotionDecision>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "collision candidate failed its promotion gate and no "
                "last-good selection exists to retain"));
    }
    CollisionPromotionDecision decision;
    decision.selected_candidate_digest = last_good_candidate_digest.value();
    decision.promoted_new_candidate = false;
    decision.retained_last_good = true;
    return core::Result<CollisionPromotionDecision>::success(decision);
}

} // namespace carto::collision
