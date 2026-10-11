#include <carto/collision/family.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::collision {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_COLLISION_VARIANT_SELECTION";
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

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool valid_family(CollisionRepresentationFamily family) {
    switch (family) {
    case CollisionRepresentationFamily::primitive:
    case CollisionRepresentationFamily::compound:
    case CollisionRepresentationFamily::convex_proxy:
    case CollisionRepresentationFamily::triangle_mesh:
        return true;
    }
    return false;
}

bool valid_purpose(CollisionPurpose purpose) {
    switch (purpose) {
    case CollisionPurpose::gameplay_query:
    case CollisionPurpose::projectile:
    case CollisionPurpose::vehicle:
    case CollisionPurpose::debris:
    case CollisionPurpose::character:
        return true;
    }
    return false;
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

bool valid_disposition(CollisionCandidateDisposition disposition) {
    switch (disposition) {
    case CollisionCandidateDisposition::selected:
    case CollisionCandidateDisposition::rejected_policy:
    case CollisionCandidateDisposition::failed:
    case CollisionCandidateDisposition::stale:
    case CollisionCandidateDisposition::cancelled:
        return true;
    }
    return false;
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

bool read_quoted(std::istream& input, std::string& value) {
    return static_cast<bool>(input >> std::quoted(value));
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

// Optional strings encode as `-`, everything else as a quoted string.
bool read_optional_text(std::istream& input, std::optional<std::string>& value) {
    input >> std::ws;
    const int next = input.peek();
    if (next == std::char_traits<char>::eof()) return false;
    if (next == '-') {
        input.get();
        value = std::nullopt;
        return true;
    }
    std::string decoded;
    if (!(input >> std::quoted(decoded))) return false;
    value = std::move(decoded);
    return true;
}

// Optional indices encode as `-`, everything else as an unsigned integer.
bool read_optional_index(std::istream& input, std::optional<std::uint32_t>& value) {
    input >> std::ws;
    const int next = input.peek();
    if (next == std::char_traits<char>::eof()) return false;
    if (next == '-') {
        input.get();
        value = std::nullopt;
        return true;
    }
    std::uint32_t parsed = 0U;
    if (!read_integer(input, parsed)) return false;
    value = parsed;
    return true;
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

CollisionRepresentationFamily parse_family(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(CollisionRepresentationFamily::triangle_mesh);
         ++value) {
        const auto family = static_cast<CollisionRepresentationFamily>(value);
        if (collision_representation_family_name(family) == text) return family;
    }
    return static_cast<CollisionRepresentationFamily>(0xFFU);
}

CollisionPurpose parse_purpose(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(CollisionPurpose::character);
         ++value) {
        const auto purpose = static_cast<CollisionPurpose>(value);
        if (collision_purpose_name(purpose) == text) return purpose;
    }
    return static_cast<CollisionPurpose>(0xFFU);
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

CollisionCandidateDisposition parse_disposition(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(CollisionCandidateDisposition::cancelled);
         ++value) {
        const auto disposition = static_cast<CollisionCandidateDisposition>(value);
        if (collision_candidate_disposition_name(disposition) == text) return disposition;
    }
    return static_cast<CollisionCandidateDisposition>(0xFFU);
}

} // namespace

std::string_view collision_representation_family_name(
    CollisionRepresentationFamily family) noexcept {
    switch (family) {
    case CollisionRepresentationFamily::primitive: return "primitive";
    case CollisionRepresentationFamily::compound: return "compound";
    case CollisionRepresentationFamily::convex_proxy: return "convex_proxy";
    case CollisionRepresentationFamily::triangle_mesh: return "triangle_mesh";
    }
    return "unknown";
}

std::string_view collision_purpose_name(CollisionPurpose purpose) noexcept {
    switch (purpose) {
    case CollisionPurpose::gameplay_query: return "gameplay_query";
    case CollisionPurpose::projectile: return "projectile";
    case CollisionPurpose::vehicle: return "vehicle";
    case CollisionPurpose::debris: return "debris";
    case CollisionPurpose::character: return "character";
    }
    return "unknown";
}

std::string_view collision_receipt_status_name(
    CollisionReceiptStatus status) noexcept {
    switch (status) {
    case CollisionReceiptStatus::admitted: return "admitted";
    case CollisionReceiptStatus::failed: return "failed";
    case CollisionReceiptStatus::stale: return "stale";
    case CollisionReceiptStatus::cancelled: return "cancelled";
    }
    return "unknown";
}

std::string_view collision_candidate_disposition_name(
    CollisionCandidateDisposition disposition) noexcept {
    switch (disposition) {
    case CollisionCandidateDisposition::selected: return "selected";
    case CollisionCandidateDisposition::rejected_policy: return "rejected_policy";
    case CollisionCandidateDisposition::failed: return "failed";
    case CollisionCandidateDisposition::stale: return "stale";
    case CollisionCandidateDisposition::cancelled: return "cancelled";
    }
    return "unknown";
}

core::Result<void> CollisionVariantBasis::validate() const {
    if (!safe_text(source_identity, kMaxTextBytes) ||
        !safe_text(algorithm, kMaxTextBytes) ||
        !safe_text(target_profile_identity, kMaxTextBytes) ||
        !safe_text(admission_policy_identity, kMaxTextBytes) ||
        !safe_text(hardware_profile, kMaxTextBytes)) {
        return core::Result<void>::failure(invalid(
            "collision variant basis text fields are invalid"));
    }
    if (source_revision.value() == 0U) {
        return core::Result<void>::failure(validation(
            "collision variant basis must bind a committed source revision"));
    }
    if (source_digest.is_zero() || settings_digest.is_zero() ||
        target_profile_digest.is_zero() || admission_policy_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "collision variant basis must bind every evidence digest"));
    }
    if (candidate_limit == 0U || candidate_limit > CollisionVariantSelection::kMaxCandidates) {
        return core::Result<void>::failure(validation(
            "collision variant candidate limit is outside its bounded range"));
    }
    return core::Result<void>::success();
}

std::string CollisionVariantBasis::canonical() const {
    std::ostringstream output;
    output << "SOURCE " << std::quoted(source_identity)
           << " REVISION " << source_revision.value()
           << " SOURCE_DIGEST " << source_digest.hex()
           << " ALGORITHM " << std::quoted(algorithm)
           << " SETTINGS_DIGEST " << settings_digest.hex()
           << " PROFILE " << std::quoted(target_profile_identity)
           << " PROFILE_DIGEST " << target_profile_digest.hex()
           << " POLICY " << std::quoted(admission_policy_identity)
           << " POLICY_DIGEST " << admission_policy_digest.hex()
           << " HARDWARE " << std::quoted(hardware_profile)
           << " CANDIDATE_LIMIT " << candidate_limit;
    return output.str();
}

core::Result<void> CollisionVariantCandidate::validate() const {
    if (!valid_family(family)) {
        return core::Result<void>::failure(invalid(
            "collision variant family is invalid"));
    }
    if (!valid_purpose(purpose)) {
        return core::Result<void>::failure(invalid(
            "collision variant purpose is invalid"));
    }
    if (candidate_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "collision variant candidate must be digest-bound"));
    }
    if (auto result = measured_error.validate(); !result) return result;
    if (!valid_disposition(disposition)) {
        return core::Result<void>::failure(invalid(
            "collision variant disposition is invalid"));
    }
    if (disposition == CollisionCandidateDisposition::selected) {
        if (!reason_code.empty()) {
            return core::Result<void>::failure(validation(
                "selected collision variant must not carry a reason code"));
        }
    } else if (!safe_text(reason_code, kMaxReasonBytes)) {
        return core::Result<void>::failure(validation(
            "non-selected collision variant requires a bounded reason code"));
    }
    return core::Result<void>::success();
}

std::string CollisionVariantCandidate::canonical() const {
    std::ostringstream output;
    output << "CANDIDATE "
           << collision_representation_family_name(family) << ' '
           << collision_purpose_name(purpose) << ' '
           << candidate_digest.hex() << ' '
           << resident_cost_bytes << ' '
           << collision_candidate_disposition_name(disposition) << ' ';
    if (reason_code.empty()) {
        output << '-';
    } else {
        output << std::quoted(reason_code);
    }
    output << ' ' << measured_error.canonical();
    return output.str();
}

core::Result<void> CollisionVariantSelection::insert_candidate(
    CollisionVariantCandidate candidate) {
    if (candidates.size() >= kMaxCandidates) {
        return core::Result<void>::failure(validation(
            "collision variant selection has reached its bounded candidate count"));
    }
    if (auto result = candidate.validate(); !result) return result;
    if (candidate.disposition == CollisionCandidateDisposition::selected &&
        std::any_of(candidates.begin(), candidates.end(),
                [](const CollisionVariantCandidate& existing) {
                    return existing.disposition ==
                        CollisionCandidateDisposition::selected;
                })) {
        return core::Result<void>::failure(validation(
            "collision variant selection admits at most one selected candidate"));
    }
    candidates.push_back(std::move(candidate));
    return core::Result<void>::success();
}

const CollisionVariantCandidate* CollisionVariantSelection::selected_candidate()
    const noexcept {
    if (!selected_index.has_value() ||
        selected_index.value() >= candidates.size()) {
        return nullptr;
    }
    return &candidates[selected_index.value()];
}

core::Result<void> CollisionVariantSelection::validate() const {
    if (auto result = basis.validate(); !result) return result;
    if (!valid_status(status)) {
        return core::Result<void>::failure(invalid(
            "collision variant selection status is invalid"));
    }
    if (candidates.empty() || candidates.size() > kMaxCandidates) {
        return core::Result<void>::failure(validation(
            "collision variant selection requires a bounded non-empty candidate set"));
    }
    for (const CollisionVariantCandidate& candidate : candidates) {
        if (auto result = candidate.validate(); !result) return result;
    }
    const bool has_selected = std::any_of(
        candidates.begin(), candidates.end(),
        [](const CollisionVariantCandidate& candidate) {
            return candidate.disposition == CollisionCandidateDisposition::selected;
        });
    if (has_selected != selected_index.has_value() ||
        (status == CollisionReceiptStatus::admitted) != selected_index.has_value()) {
        return core::Result<void>::failure(validation(
            "collision variant selection status and selected index disagree"));
    }
    if (selected_index.has_value() &&
        candidates[selected_index.value()].disposition !=
            CollisionCandidateDisposition::selected) {
        return core::Result<void>::failure(validation(
            "collision variant selected index does not reference the selected candidate"));
    }
    if (decision_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "collision variant selection must be digest-bound via refresh_digest"));
    }
    return core::Result<void>::success();
}

std::string CollisionVariantSelection::canonical_without_digest() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "BASIS " << basis.canonical() << '\n';
    output << "STATUS " << collision_receipt_status_name(status) << " SELECTED ";
    if (selected_index.has_value()) {
        output << selected_index.value();
    } else {
        output << '-';
    }
    output << " CANDIDATES " << candidates.size() << '\n';
    for (const CollisionVariantCandidate& candidate : candidates) {
        output << candidate.canonical() << '\n';
    }
    return output.str();
}

std::string CollisionVariantSelection::canonical() const {
    return canonical_without_digest() + "DIGEST " + decision_digest.hex() + "\n";
}

core::Result<void> CollisionVariantSelection::refresh_digest() {
    decision_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string CollisionVariantSelection::serialize() const {
    return canonical() + "END\n";
}

core::Result<CollisionVariantSelection> CollisionVariantSelection::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic || !read_integer(input, version) ||
        version != kSchemaVersion) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection framing or schema version is invalid"));
    }

    CollisionVariantSelection selection;
    std::uint64_t source_revision = 0U;
    if (!require_token(input, "BASIS") ||
        !require_token(input, "SOURCE") ||
        !read_quoted(input, selection.basis.source_identity) ||
        !require_token(input, "REVISION") ||
        !read_integer(input, source_revision) ||
        !require_token(input, "SOURCE_DIGEST") ||
        !read_digest(input, selection.basis.source_digest) ||
        !require_token(input, "ALGORITHM") ||
        !read_quoted(input, selection.basis.algorithm) ||
        !require_token(input, "SETTINGS_DIGEST") ||
        !read_digest(input, selection.basis.settings_digest) ||
        !require_token(input, "PROFILE") ||
        !read_quoted(input, selection.basis.target_profile_identity) ||
        !require_token(input, "PROFILE_DIGEST") ||
        !read_digest(input, selection.basis.target_profile_digest) ||
        !require_token(input, "POLICY") ||
        !read_quoted(input, selection.basis.admission_policy_identity) ||
        !require_token(input, "POLICY_DIGEST") ||
        !read_digest(input, selection.basis.admission_policy_digest) ||
        !require_token(input, "HARDWARE") ||
        !read_quoted(input, selection.basis.hardware_profile) ||
        !require_token(input, "CANDIDATE_LIMIT") ||
        !read_integer(input, selection.basis.candidate_limit)) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection basis is malformed"));
    }
    selection.basis.source_revision = core::Revision{source_revision};

    std::string status;
    std::size_t candidate_count = 0U;
    if (!require_token(input, "STATUS") || !(input >> status) ||
        !require_token(input, "SELECTED") ||
        !read_optional_index(input, selection.selected_index) ||
        !require_token(input, "CANDIDATES") ||
        !read_count(input, kMaxCandidates, candidate_count)) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection header is malformed"));
    }
    selection.status = parse_status(status);
    if (!valid_status(selection.status)) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection status is unknown"));
    }
    if (selection.selected_index.has_value() &&
        selection.selected_index.value() >= candidate_count) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection selected index is out of range"));
    }

    selection.candidates.reserve(candidate_count);
    for (std::size_t index = 0U; index < candidate_count; ++index) {
        CollisionVariantCandidate candidate;
        std::string family;
        std::string purpose;
        std::string disposition;
        std::optional<std::string> reason;
        if (!require_token(input, "CANDIDATE") || !(input >> family) ||
            !(input >> purpose) || !read_digest(input, candidate.candidate_digest) ||
            !read_integer(input, candidate.resident_cost_bytes) ||
            !(input >> disposition) ||
            !read_optional_text(input, reason) ||
            !read_metrics(input, candidate.measured_error)) {
            return core::Result<CollisionVariantSelection>::failure(parse_error(
                "collision variant selection candidate is malformed"));
        }
        candidate.reason_code = reason.value_or("");
        candidate.family = parse_family(family);
        candidate.purpose = parse_purpose(purpose);
        candidate.disposition = parse_disposition(disposition);
        selection.candidates.push_back(std::move(candidate));
    }

    if (!require_token(input, "DIGEST") ||
        !read_digest(input, selection.decision_digest) ||
        !require_token(input, "END")) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection trailer is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CollisionVariantSelection>::failure(parse_error(
            "collision variant selection has trailing data"));
    }

    if (auto result = selection.validate(); !result) {
        return core::Result<CollisionVariantSelection>::failure(result.error());
    }
    const assets::Sha256Digest expected =
        digest_text(selection.canonical_without_digest());
    if (expected != selection.decision_digest) {
        return core::Result<CollisionVariantSelection>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "collision variant selection digest does not match its content"));
    }
    return core::Result<CollisionVariantSelection>::success(std::move(selection));
}

} // namespace carto::collision
