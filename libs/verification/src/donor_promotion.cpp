#include <carto/verification/donor_promotion.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::verification {
namespace {

constexpr std::string_view kLedgerMagic =
    "CARTOGRAPHER_DONOR_PROMOTION_LEDGER";
constexpr std::string_view kReceiptMagic =
    "CARTOGRAPHER_DONOR_PROMOTION_RECEIPT";
constexpr std::string_view kGreenfieldAbsenceReason =
    "greenfield Cartographer implementation; no donor evidence";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte <= 0x7eU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool safe_identifier(std::string_view value, std::size_t maximum) {
    if (!safe_text(value, maximum)) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
            byte == '-';
    });
}

bool contains_forbidden_private_material(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        lowered.push_back(static_cast<char>(std::tolower(byte)));
    }
    constexpr std::array<std::string_view, 12> forbidden{
        "-----begin", "bearer ", "api_key", "access_token", "client_secret",
        "password=", "file://", "http://", "https://", "e:\\", "c:\\",
        "/users/"};
    return std::any_of(forbidden.begin(), forbidden.end(),
        [&lowered](std::string_view token) {
            return lowered.find(token) != std::string::npos;
        });
}

bool safe_public_text(std::string_view value, std::size_t maximum,
    bool allow_empty = false) {
    return safe_text(value, maximum, allow_empty) &&
        !contains_forbidden_private_material(value);
}

bool safe_logical_reference(std::string_view value, std::size_t maximum) {
    if (!safe_public_text(value, maximum) || value.front() == '/' ||
        value.front() == '\\' || value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t segment_start = 0U;
    while (segment_start <= value.size()) {
        const auto separator = value.find('/', segment_start);
        const auto segment_end = separator == std::string_view::npos
            ? value.size() : separator;
        const auto segment = value.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (separator == std::string_view::npos) break;
        segment_start = separator + 1U;
    }
    return true;
}

bool valid_donor_class(DonorClass value) {
    return value == DonorClass::promote_implementation ||
        value == DonorClass::borrow_pattern || value == DonorClass::greenfield;
}

bool valid_disposition(PromotionDisposition value) {
    return value == PromotionDisposition::not_promoted ||
        value == PromotionDisposition::architecture_only ||
        value == PromotionDisposition::reimplemented ||
        value == PromotionDisposition::copied_adapted;
}

bool valid_status(PromotionStatus value) {
    return value == PromotionStatus::observed ||
        value == PromotionStatus::planned || value == PromotionStatus::adapting ||
        value == PromotionStatus::accepted || value == PromotionStatus::rejected;
}

bool valid_commit(std::string_view value) {
    if (value.size() != 40U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= '0' && byte <= '9') ||
            (byte >= 'a' && byte <= 'f');
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

bool read_count(std::istream& input, std::size_t maximum,
    std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) ||
        encoded > maximum ||
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
        return core::Result<assets::Sha256Digest>::failure(validation(
            "donor promotion receipt digest is invalid"));
    }
    return digest;
}

bool sorted_unique(const std::vector<std::string>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

core::Result<void> validate_list(const std::vector<std::string>& values,
    std::string_view name) {
    if (values.empty() || values.size() > DonorPromotionReceipt::kMaxListEntries) {
        return core::Result<void>::failure(validation(
            std::string(name) + " must be non-empty and bounded"));
    }
    for (const auto& value : values) {
        if (!safe_public_text(value, DonorPromotionReceipt::kMaxListItemBytes)) {
            return core::Result<void>::failure(validation(
                std::string(name) + " contains unsafe or oversized text"));
        }
    }
    if (!sorted_unique(values)) {
        return core::Result<void>::failure(validation(
            std::string(name) + " must be sorted and unique"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_receipt_content(
    const DonorPromotionReceipt& receipt, bool check_digest) {
    if (receipt.sequence == 0U) {
        return core::Result<void>::failure(validation(
            "donor promotion receipt sequence is zero"));
    }
    if (!safe_identifier(receipt.promotion_id,
            DonorPromotionReceipt::kMaxPromotionIdBytes)) {
        return core::Result<void>::failure(validation(
            "donor promotion identifier is empty, oversized, or unstable"));
    }
    if (!safe_identifier(receipt.target_module,
            DonorPromotionReceipt::kMaxTargetModuleBytes)) {
        return core::Result<void>::failure(validation(
            "donor promotion target module is empty, oversized, or unstable"));
    }
    if (!valid_donor_class(receipt.donor_class) ||
        !valid_disposition(receipt.disposition) || !valid_status(receipt.status)) {
        return core::Result<void>::failure(validation(
            "donor promotion receipt contains an unknown classification"));
    }
    if (auto result = receipt.donor.validate(); !result) return result;
    if (!safe_public_text(receipt.license_basis,
            DonorPromotionReceipt::kMaxLicenseBasisBytes)) {
        return core::Result<void>::failure(validation(
            "donor promotion license basis is missing or unsafe"));
    }
    if (auto result = validate_list(receipt.semantic_changes, "semantic changes");
        !result) return result;
    if (auto result = validate_list(receipt.removed_authorities,
            "removed authorities"); !result) return result;
    if (auto result = validate_list(receipt.tests, "tests"); !result) return result;
    if (auto result = validate_list(receipt.fixtures, "fixtures"); !result) {
        return result;
    }
    if (!receipt.non_claims.empty()) {
        if (auto result = validate_list(receipt.non_claims, "non-claims"); !result) {
            return result;
        }
    }
    if (!safe_public_text(receipt.decision, DonorPromotionReceipt::kMaxDecisionBytes)) {
        return core::Result<void>::failure(validation(
            "donor promotion decision is missing or unsafe"));
    }

    if (receipt.donor_class == DonorClass::greenfield) {
        if (!receipt.donor.repository_id.empty() || !receipt.donor.commit.empty() ||
            !receipt.donor.logical_references.empty() ||
            receipt.donor.commit_absence_reason != kGreenfieldAbsenceReason) {
            return core::Result<void>::failure(validation(
                "greenfield promotion must carry explicit absence of donor evidence"));
        }
        if (receipt.disposition != PromotionDisposition::not_promoted ||
            receipt.status == PromotionStatus::accepted) {
            return core::Result<void>::failure(validation(
                "greenfield work cannot be represented as accepted donor promotion"));
        }
    } else if (receipt.donor.repository_id.empty() ||
        receipt.donor.logical_references.empty()) {
        return core::Result<void>::failure(validation(
            "a donor promotion requires a repository identity and logical references"));
    }

    if (receipt.donor_class == DonorClass::borrow_pattern &&
        receipt.disposition == PromotionDisposition::copied_adapted) {
        return core::Result<void>::failure(validation(
            "a borrowed architecture pattern cannot claim copied source promotion"));
    }
    if (receipt.disposition == PromotionDisposition::not_promoted &&
        receipt.non_claims.empty()) {
        return core::Result<void>::failure(validation(
            "non-promoted work requires an explicit non-claim"));
    }
    if (receipt.status == PromotionStatus::rejected) {
        if (receipt.disposition != PromotionDisposition::not_promoted ||
            receipt.non_claims.empty()) {
            return core::Result<void>::failure(validation(
                "rejected promotion must be a bounded explicit non-claim"));
        }
    }
    if (receipt.status == PromotionStatus::observed &&
        (receipt.disposition == PromotionDisposition::reimplemented ||
            receipt.disposition == PromotionDisposition::copied_adapted)) {
        return core::Result<void>::failure(validation(
            "observed donor evidence cannot claim an accepted implementation"));
    }
    if (receipt.status == PromotionStatus::accepted) {
        if (receipt.donor_class == DonorClass::greenfield ||
            (receipt.disposition != PromotionDisposition::reimplemented &&
                receipt.disposition != PromotionDisposition::copied_adapted) ||
            receipt.tests.empty() || receipt.fixtures.empty()) {
            return core::Result<void>::failure(validation(
                "accepted donor promotion requires an implemented disposition and fixtures"));
        }
    }
    if (check_digest) {
        if (receipt.receipt_digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "donor promotion receipt digest is not bound"));
        }
        if (digest_text(receipt.canonical_without_digest()) != receipt.receipt_digest) {
            return core::Result<void>::failure(validation(
                "donor promotion receipt digest does not match canonical content"));
        }
    }
    return core::Result<void>::success();
}

void append_list(std::ostringstream& output, std::string_view count_token,
    std::string_view item_token, const std::vector<std::string>& values) {
    output << count_token << ' ' << values.size() << '\n';
    for (const auto& value : values) {
        output << item_token << ' ' << std::quoted(value) << '\n';
    }
}

void append_receipt_fields(std::ostringstream& output,
    const DonorPromotionReceipt& receipt, bool include_digest) {
    output << "SEQ " << receipt.sequence << '\n';
    output << "ID " << std::quoted(receipt.promotion_id) << '\n';
    output << "TARGET " << std::quoted(receipt.target_module) << '\n';
    output << "CLASS " << static_cast<unsigned>(receipt.donor_class) << '\n';
    output << "DISPOSITION " << static_cast<unsigned>(receipt.disposition) << '\n';
    output << "STATUS " << static_cast<unsigned>(receipt.status) << '\n';
    output << "DONOR_REPOSITORY " << std::quoted(receipt.donor.repository_id) << '\n';
    output << "DONOR_COMMIT " << std::quoted(receipt.donor.commit) << '\n';
    output << "DONOR_ABSENCE "
           << std::quoted(receipt.donor.commit_absence_reason) << '\n';
    append_list(output, "REFERENCES", "REFERENCE", receipt.donor.logical_references);
    output << "LICENSE " << std::quoted(receipt.license_basis) << '\n';
    append_list(output, "CHANGES", "CHANGE", receipt.semantic_changes);
    append_list(output, "REMOVED_AUTHORITIES", "REMOVED",
        receipt.removed_authorities);
    append_list(output, "TESTS", "TEST", receipt.tests);
    append_list(output, "FIXTURES", "FIXTURE", receipt.fixtures);
    if (receipt.non_claims.empty()) {
        output << "NON_CLAIMS 0\n";
    } else {
        append_list(output, "NON_CLAIMS", "NON_CLAIM", receipt.non_claims);
    }
    output << "DECISION " << std::quoted(receipt.decision) << '\n';
    if (include_digest) output << "DIGEST " << receipt.receipt_digest.hex() << '\n';
}

template <typename Reader>
bool read_list(std::istream& input, std::string_view count_token,
    std::string_view item_token, std::vector<std::string>& values,
    Reader&& reader) {
    std::size_t count = 0U;
    if (!require_token(input, count_token) ||
        !read_count(input, DonorPromotionReceipt::kMaxListEntries, count)) {
        return false;
    }
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        if (!require_token(input, item_token) || !reader(input, values)) return false;
    }
    return true;
}

bool read_quoted_item(std::istream& input, std::vector<std::string>& values) {
    std::string value;
    if (!(input >> std::quoted(value))) return false;
    values.push_back(std::move(value));
    return true;
}

core::Result<DonorPromotionReceipt> read_receipt_body(std::istream& input) {
    DonorPromotionReceipt receipt;
    unsigned encoded = 0U;
    std::string encoded_digest;
    if (!require_token(input, "SEQ") || !read_unsigned(input, receipt.sequence) ||
        !require_token(input, "ID") || !(input >> std::quoted(receipt.promotion_id)) ||
        !require_token(input, "TARGET") ||
        !(input >> std::quoted(receipt.target_module)) ||
        !require_token(input, "CLASS") || !read_unsigned(input, encoded) ||
        encoded > 255U) {
        return core::Result<DonorPromotionReceipt>::failure(validation(
            "donor promotion receipt identity is invalid"));
    }
    receipt.donor_class = static_cast<DonorClass>(encoded);
    if (!require_token(input, "DISPOSITION") || !read_unsigned(input, encoded) ||
        encoded > 255U) {
        return core::Result<DonorPromotionReceipt>::failure(validation(
            "donor promotion receipt disposition is invalid"));
    }
    receipt.disposition = static_cast<PromotionDisposition>(encoded);
    if (!require_token(input, "STATUS") || !read_unsigned(input, encoded) ||
        encoded > 255U || !require_token(input, "DONOR_REPOSITORY") ||
        !(input >> std::quoted(receipt.donor.repository_id)) ||
        !require_token(input, "DONOR_COMMIT") ||
        !(input >> std::quoted(receipt.donor.commit)) ||
        !require_token(input, "DONOR_ABSENCE") ||
        !(input >> std::quoted(receipt.donor.commit_absence_reason)) ||
        !read_list(input, "REFERENCES", "REFERENCE",
            receipt.donor.logical_references, read_quoted_item) ||
        !require_token(input, "LICENSE") ||
        !(input >> std::quoted(receipt.license_basis)) ||
        !read_list(input, "CHANGES", "CHANGE", receipt.semantic_changes,
            read_quoted_item) ||
        !read_list(input, "REMOVED_AUTHORITIES", "REMOVED",
            receipt.removed_authorities, read_quoted_item) ||
        !read_list(input, "TESTS", "TEST", receipt.tests, read_quoted_item) ||
        !read_list(input, "FIXTURES", "FIXTURE", receipt.fixtures,
            read_quoted_item) ||
        !read_list(input, "NON_CLAIMS", "NON_CLAIM", receipt.non_claims,
            read_quoted_item) ||
        !require_token(input, "DECISION") ||
        !(input >> std::quoted(receipt.decision)) ||
        !require_token(input, "DIGEST") || !(input >> encoded_digest) ||
        !require_token(input, "END")) {
        return core::Result<DonorPromotionReceipt>::failure(validation(
            "donor promotion receipt body is invalid"));
    }
    receipt.status = static_cast<PromotionStatus>(encoded);
    const auto digest = parse_digest(encoded_digest);
    if (!digest) return core::Result<DonorPromotionReceipt>::failure(digest.error());
    receipt.receipt_digest = digest.value();
    if (std::string extra; input >> extra) {
        return core::Result<DonorPromotionReceipt>::failure(validation(
            "donor promotion receipt contains trailing data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<DonorPromotionReceipt>::failure(result.error());
    }
    return core::Result<DonorPromotionReceipt>::success(std::move(receipt));
}

} // namespace

core::Result<void> DonorEvidence::validate() const {
    if (!repository_id.empty() &&
        !safe_identifier(repository_id, 128U)) {
        return core::Result<void>::failure(validation(
            "donor repository identity is empty, oversized, or unstable"));
    }
    if (!commit.empty()) {
        if (!valid_commit(commit) || !commit_absence_reason.empty()) {
            return core::Result<void>::failure(validation(
                "donor commit must be a lower-case verified Git identity"));
        }
    } else if (!safe_public_text(commit_absence_reason, 512U)) {
        return core::Result<void>::failure(validation(
            "missing donor commit requires a safe explicit absence reason"));
    }
    if (logical_references.size() > DonorPromotionReceipt::kMaxListEntries) {
        return core::Result<void>::failure(validation(
            "donor logical references exceed their bounded capacity"));
    }
    for (const auto& reference : logical_references) {
        if (!safe_logical_reference(reference,
                DonorPromotionReceipt::kMaxListItemBytes)) {
            return core::Result<void>::failure(validation(
                "donor logical reference is absolute, unsafe, or oversized"));
        }
    }
    if (!sorted_unique(logical_references)) {
        return core::Result<void>::failure(validation(
            "donor logical references must be sorted and unique"));
    }
    return core::Result<void>::success();
}

core::Result<void> DonorPromotionReceipt::validate() const {
    return validate_receipt_content(*this, true);
}

std::string DonorPromotionReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << 1U << '\n';
    append_receipt_fields(output, *this, false);
    output << "END\n";
    return output.str();
}

core::Result<void> DonorPromotionReceipt::refresh_digest() {
    if (auto result = validate_receipt_content(*this, false); !result) return result;
    receipt_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string DonorPromotionReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << 1U << '\n';
    append_receipt_fields(output, *this, true);
    output << "END\n";
    return output.str();
}

core::Result<DonorPromotionReceipt> DonorPromotionReceipt::deserialize(
    std::string_view text) {
    if (text.size() > DonorPromotionLedger::kMaxSerializedBytes) {
        return core::Result<DonorPromotionReceipt>::failure(validation(
            "donor promotion receipt exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kReceiptMagic) || !read_unsigned(input, version) ||
        version != 1U) {
        return core::Result<DonorPromotionReceipt>::failure(validation(
            "donor promotion receipt header is invalid"));
    }
    return read_receipt_body(input);
}

core::Result<void> DonorPromotionLedger::append(DonorPromotionReceipt receipt) {
    if (records_.size() >= kMaxRecords) {
        return core::Result<void>::failure(validation(
            "donor promotion ledger has reached its bounded capacity"));
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<void>::failure(validation(
            "donor promotion ledger sequence space is exhausted"));
    }
    if (std::any_of(records_.begin(), records_.end(),
            [&receipt](const DonorPromotionReceipt& existing) {
                return existing.promotion_id == receipt.promotion_id;
            })) {
        return core::Result<void>::failure(validation(
            "donor promotion ledger cannot reuse a promotion identifier"));
    }
    receipt.sequence = next_sequence_;
    receipt.receipt_digest = assets::Sha256Digest{};
    if (auto result = validate_receipt_content(receipt, false); !result) {
        return result;
    }
    receipt.receipt_digest = digest_text(receipt.canonical_without_digest());
    if (auto result = receipt.validate(); !result) return result;
    records_.push_back(std::move(receipt));
    ++next_sequence_;
    return core::Result<void>::success();
}

core::Result<void> DonorPromotionLedger::validate() const {
    if (records_.size() > kMaxRecords) {
        return core::Result<void>::failure(validation(
            "donor promotion ledger exceeds its bounded capacity"));
    }
    std::set<std::string> promotion_ids;
    std::uint64_t previous_sequence = 0U;
    for (const auto& record : records_) {
        if (auto result = record.validate(); !result) return result;
        if (record.sequence <= previous_sequence) {
            return core::Result<void>::failure(validation(
                "donor promotion ledger records are not in append order"));
        }
        previous_sequence = record.sequence;
        if (!promotion_ids.insert(record.promotion_id).second) {
            return core::Result<void>::failure(validation(
                "donor promotion ledger contains a duplicate promotion identifier"));
        }
    }
    if (!records_.empty() && previous_sequence >= next_sequence_) {
        return core::Result<void>::failure(validation(
            "donor promotion ledger sequence cursor is behind its records"));
    }
    return core::Result<void>::success();
}

std::string DonorPromotionLedger::serialize() const {
    std::ostringstream output;
    output << kLedgerMagic << ' ' << kSchemaVersion << '\n';
    output << "RECORDS " << records_.size() << '\n';
    for (const auto& record : records_) {
        output << "BEGIN_RECORD\n";
        append_receipt_fields(output, record, true);
        output << "END_RECORD\n";
    }
    output << "END\n";
    return output.str();
}

core::Result<DonorPromotionLedger> DonorPromotionLedger::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<DonorPromotionLedger>::failure(validation(
            "donor promotion ledger exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    std::size_t count = 0U;
    if (!require_token(input, kLedgerMagic) || !read_unsigned(input, version) ||
        version != kSchemaVersion || !require_token(input, "RECORDS") ||
        !read_count(input, kMaxRecords, count)) {
        return core::Result<DonorPromotionLedger>::failure(validation(
            "donor promotion ledger header is invalid"));
    }

    DonorPromotionLedger ledger;
    ledger.records_.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        if (!require_token(input, "BEGIN_RECORD")) {
            return core::Result<DonorPromotionLedger>::failure(validation(
                "donor promotion ledger record marker is invalid"));
        }
        input >> std::ws;
        std::ostringstream receipt_text;
        receipt_text << kReceiptMagic << " 1\n";
        std::string line;
        bool found_end = false;
        while (std::getline(input, line)) {
            if (line == "END_RECORD") {
                found_end = true;
                break;
            }
            receipt_text << line << '\n';
        }
        if (!found_end) {
            return core::Result<DonorPromotionLedger>::failure(validation(
                "donor promotion ledger record is unterminated"));
        }
        receipt_text << "END\n";
        const auto receipt = DonorPromotionReceipt::deserialize(receipt_text.str());
        if (!receipt) {
            return core::Result<DonorPromotionLedger>::failure(
                receipt.error().with_context("donor promotion ledger record"));
        }
        ledger.records_.push_back(receipt.value());
    }
    if (!require_token(input, "END")) {
        return core::Result<DonorPromotionLedger>::failure(validation(
            "donor promotion ledger terminator is invalid"));
    }
    if (std::string extra; input >> extra) {
        return core::Result<DonorPromotionLedger>::failure(validation(
            "donor promotion ledger contains trailing data"));
    }
    if (!ledger.records_.empty()) {
        if (ledger.records_.back().sequence ==
            std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<DonorPromotionLedger>::failure(validation(
                "donor promotion ledger sequence cursor cannot advance"));
        }
        ledger.next_sequence_ = ledger.records_.back().sequence + 1U;
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<DonorPromotionLedger>::failure(result.error());
    }
    return core::Result<DonorPromotionLedger>::success(std::move(ledger));
}

core::Result<assets::Sha256Digest> DonorPromotionLedger::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

} // namespace carto::verification
