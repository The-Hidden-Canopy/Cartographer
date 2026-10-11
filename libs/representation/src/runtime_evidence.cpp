#include <carto/representation/runtime_evidence.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <istream>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::representation {
namespace {

constexpr std::string_view kLedgerMagic = "CARTOGRAPHER_REPRESENTATION_EVIDENCE_LEDGER";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
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

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

double canonical_double(double value) {
    return value == 0.0 ? 0.0 : value;
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

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token)) return false;
    if (token.find_first_not_of("+-0123456789.eE") != std::string::npos) {
        return false;
    }
    std::istringstream token_input(token);
    token_input >> std::setprecision(17) >> value;
    return static_cast<bool>(token_input) &&
        token_input.peek() == std::istringstream::traits_type::eof();
}

bool read_digest(std::istream& input, assets::Sha256Digest& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    value = parsed.value();
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

void write_record(std::ostringstream& output,
                  const RepresentationRuntimeEvidence& record) {
    output << "RECORD " << record.family.value << ' ' << record.variant.value << ' '
           << std::quoted(record.hardware_profile) << ' '
           << record.profile_digest.hex() << ' ' << record.source_revision.value()
           << ' ' << evidence_status_name(record.status) << ' '
           << record.measured_resident_bytes << ' ' << std::setprecision(17)
           << canonical_double(record.observed_error.max_position_error) << ' '
           << canonical_double(record.observed_error.max_silhouette_error) << ' '
           << canonical_double(record.observed_error.max_normal_error) << ' '
           << canonical_double(record.observed_error.max_radiance_error) << ' '
           << record.evidence_digest.hex() << '\n';
}

bool read_record(std::istream& input, RepresentationRuntimeEvidence& record) {
    std::string status_name;
    std::uint64_t revision = 0U;
    if (!require_token(input, "RECORD") || !read_integer(input, record.family.value) ||
        !read_integer(input, record.variant.value) ||
        !(input >> std::quoted(record.hardware_profile)) ||
        !read_digest(input, record.profile_digest) ||
        !read_integer(input, revision) || !(input >> status_name) ||
        !read_integer(input, record.measured_resident_bytes) ||
        !read_double(input, record.observed_error.max_position_error) ||
        !read_double(input, record.observed_error.max_silhouette_error) ||
        !read_double(input, record.observed_error.max_normal_error) ||
        !read_double(input, record.observed_error.max_radiance_error) ||
        !read_digest(input, record.evidence_digest)) {
        return false;
    }
    const auto status = evidence_status_from_name(status_name);
    if (!status) return false;
    record.source_revision = core::Revision{revision};
    record.status = status.value();
    return true;
}

} // namespace

std::string_view evidence_status_name(EvidenceStatus status) noexcept {
    switch (status) {
    case EvidenceStatus::admitted: return "admitted";
    case EvidenceStatus::failed: return "failed";
    case EvidenceStatus::stale: return "stale";
    }
    return "unknown";
}

core::Result<EvidenceStatus> evidence_status_from_name(std::string_view name) {
    for (const auto status :
         {EvidenceStatus::admitted, EvidenceStatus::failed, EvidenceStatus::stale}) {
        if (evidence_status_name(status) == name) {
            return core::Result<EvidenceStatus>::success(status);
        }
    }
    return core::Result<EvidenceStatus>::failure(
        invalid("evidence status name is unsupported"));
}

core::Result<void> RepresentationRuntimeEvidence::validate() const {
    if (!family || !variant) {
        return core::Result<void>::failure(invalid(
            "runtime evidence family or variant identity is zero"));
    }
    if (!safe_text(hardware_profile, RepresentationEvidenceLedger::kMaxProfileBytes)) {
        return core::Result<void>::failure(invalid(
            "runtime evidence hardware profile is empty, unsafe, or exceeds its bound"));
    }
    if (profile_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "runtime evidence must bind a hardware profile digest"));
    }
    if (source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "runtime evidence source revision is exhausted"));
    }
    if (auto result = observed_error.validate(); !result) return result;
    if (evidence_digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "runtime evidence digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string RepresentationRuntimeEvidence::canonical_without_digest() const {
    std::ostringstream output;
    output << "runtime-evidence-v1{family=" << family.value
           << ";variant=" << variant.value << ";profile=" << hardware_profile
           << ";profile-digest=" << profile_digest.hex()
           << ";source-revision=" << source_revision.value()
           << ";status=" << evidence_status_name(status)
           << ";resident-bytes=" << measured_resident_bytes << ";error={position="
           << std::setprecision(17) << canonical_double(observed_error.max_position_error)
           << ";silhouette=" << canonical_double(observed_error.max_silhouette_error)
           << ";normal=" << canonical_double(observed_error.max_normal_error)
           << ";radiance=" << canonical_double(observed_error.max_radiance_error)
           << "}}";
    return output.str();
}

core::Result<void> RepresentationRuntimeEvidence::refresh_digest() {
    if (!family || !variant || !safe_text(
            hardware_profile, RepresentationEvidenceLedger::kMaxProfileBytes) ||
        profile_digest.is_zero() || source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "runtime evidence cannot refresh an incomplete digest"));
    }
    if (auto result = observed_error.validate(); !result) return result;
    evidence_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<void> RepresentationEvidenceLedger::append(
    RepresentationRuntimeEvidence evidence) {
    if (records_.size() >= kMaxRecords) {
        return core::Result<void>::failure(validation(
            "representation evidence ledger has reached its bounded capacity"));
    }
    if (auto result = evidence.validate(); !result) return result;
    records_.push_back(std::move(evidence));
    return core::Result<void>::success();
}

core::Result<std::optional<RepresentationRuntimeEvidence>>
RepresentationEvidenceLedger::query(
    RepresentationFamilyId family,
    RepresentationVariantId variant,
    std::string_view hardware_profile) const {
    if (!family || !variant ||
        !safe_text(hardware_profile, kMaxProfileBytes)) {
        return core::Result<std::optional<RepresentationRuntimeEvidence>>::failure(
            invalid("evidence ledger query key is incomplete"));
    }
    for (auto iterator = records_.rbegin(); iterator != records_.rend(); ++iterator) {
        if (iterator->family == family && iterator->variant == variant &&
            iterator->hardware_profile == hardware_profile) {
            if (iterator->status != EvidenceStatus::admitted) {
                return core::Result<std::optional<RepresentationRuntimeEvidence>>::
                    success(std::nullopt);
            }
            return core::Result<std::optional<RepresentationRuntimeEvidence>>::success(
                *iterator);
        }
    }
    return core::Result<std::optional<RepresentationRuntimeEvidence>>::success(
        std::nullopt);
}

core::Result<void> RepresentationEvidenceLedger::validate() const {
    if (records_.size() > kMaxRecords) {
        return core::Result<void>::failure(validation(
            "representation evidence ledger exceeds its bounded capacity"));
    }
    for (const auto& record : records_) {
        if (auto result = record.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string RepresentationEvidenceLedger::serialize() const {
    std::ostringstream output;
    output << kLedgerMagic << ' ' << kSchemaVersion << '\n';
    output << "RECORDS " << records_.size() << '\n';
    for (const auto& record : records_) {
        write_record(output, record);
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> RepresentationEvidenceLedger::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(
        digest_text(serialize()));
}

core::Result<RepresentationEvidenceLedger> RepresentationEvidenceLedger::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RepresentationEvidenceLedger>::failure(parse_error(
            "representation evidence ledger text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kLedgerMagic) {
        return core::Result<RepresentationEvidenceLedger>::failure(parse_error(
            "representation evidence ledger framing is invalid"));
    }
    std::uint32_t version = 0U;
    if (!read_integer(input, version)) {
        return core::Result<RepresentationEvidenceLedger>::failure(parse_error(
            "representation evidence ledger version is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RepresentationEvidenceLedger>::failure(version_mismatch(
            "representation evidence ledger schema version is unsupported"));
    }
    std::size_t record_count = 0U;
    if (!require_token(input, "RECORDS") || !read_count(input, kMaxRecords, record_count)) {
        return core::Result<RepresentationEvidenceLedger>::failure(parse_error(
            "representation evidence ledger record header is invalid"));
    }
    RepresentationEvidenceLedger ledger;
    for (std::size_t index = 0U; index < record_count; ++index) {
        RepresentationRuntimeEvidence record;
        if (!read_record(input, record)) {
            return core::Result<RepresentationEvidenceLedger>::failure(parse_error(
                "representation evidence ledger record is malformed"));
        }
        if (auto result = ledger.append(std::move(record)); !result) {
            return core::Result<RepresentationEvidenceLedger>::failure(result.error());
        }
    }
    std::string end_token;
    std::string trailing;
    if (!(input >> end_token) || end_token != "END" || (input >> trailing)) {
        return core::Result<RepresentationEvidenceLedger>::failure(parse_error(
            "representation evidence ledger is missing END or contains trailing data"));
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<RepresentationEvidenceLedger>::failure(result.error());
    }
    return core::Result<RepresentationEvidenceLedger>::success(std::move(ledger));
}

} // namespace carto::representation
