#include <carto/connection/port.hpp>

#include "record_io.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <span>
#include <utility>

namespace carto::connection {
namespace {

constexpr std::string_view kPortMagic = "CARTOGRAPHER_CONNECTION_PORT";
constexpr std::string_view kEvidenceMagic = "CARTOGRAPHER_PORT_COMPATIBILITY_EVIDENCE";

bool sorted_unique(const std::vector<std::string>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

bool sorted_unique(const std::vector<PortCompatibilityTag>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

template <typename Record>
core::Result<assets::Sha256Digest> digest_of(const Record& record) {
    if (auto result = record.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = record.serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<void> validate_string_list(
    const std::vector<std::string>& values,
    std::size_t maximum,
    std::string_view label) {
    if (values.size() > maximum || !sorted_unique(values)) {
        return core::Result<void>::failure(detail::validation_error(
            std::string(label) + " must be bounded, sorted, and unique"));
    }
    if (!std::all_of(values.begin(), values.end(), [](const std::string& value) {
            return detail::safe_text(value, kMaxCapabilityBytes);
        })) {
        return core::Result<void>::failure(
            detail::invalid_field(std::string(label) + " contains an invalid term"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> ReferenceFrame::validate() const {
    if (!detail::safe_text(frame_identity, kMaxFrameIdentityBytes)) {
        return core::Result<void>::failure(
            detail::invalid_field("connection port frame identity is invalid"));
    }
    if (parent_frame.has_value() &&
        (!detail::safe_text(*parent_frame, kMaxFrameIdentityBytes) ||
            *parent_frame == frame_identity)) {
        return core::Result<void>::failure(detail::invalid_field(
            "connection port parent frame reference is invalid or self-referential"));
    }
    if (!transform_to_parent.finite()) {
        return core::Result<void>::failure(detail::validation_error(
            "connection port frame transform is not finite"));
    }
    if (!detail::safe_text(coordinate_convention, kMaxConventionBytes)) {
        return core::Result<void>::failure(detail::invalid_field(
            "connection port coordinate convention reference is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PortCompatibilityTag::validate() const {
    if (!detail::safe_text(space, kMaxTagSpaceBytes) ||
        !detail::safe_text(term, kMaxTagTermBytes)) {
        return core::Result<void>::failure(
            detail::invalid_field("port compatibility tag namespace or term is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PortCapabilityContract::validate() const {
    if (auto result = validate_string_list(required, kMaxCapabilitiesPerContract, "required capabilities");
        !result) {
        return result;
    }
    return validate_string_list(offered, kMaxCapabilitiesPerContract, "offered capabilities");
}

core::Result<void> PortCompatibilityEvidence::validate() const {
    if (shared_tags.empty() || shared_tags.size() > kMaxCompatibilityTagsPerPort ||
        !sorted_unique(shared_tags)) {
        return core::Result<void>::failure(detail::validation_error(
            "compatibility evidence shared tags must be bounded, sorted, and unique"));
    }
    for (const auto& tag : shared_tags) {
        if (auto result = tag.validate(); !result) return result;
    }
    if (auto result = validate_string_list(
            first_satisfied_requirements, kMaxCapabilitiesPerContract,
            "first-side satisfied requirements");
        !result) {
        return result;
    }
    return validate_string_list(
        second_satisfied_requirements, kMaxCapabilitiesPerContract,
        "second-side satisfied requirements");
}

std::string PortCompatibilityEvidence::serialize() const {
    std::ostringstream output;
    output << kEvidenceMagic << ' ' << kPortCompatibilityEvidenceVersion << '\n';
    output << "EVIDENCE";
    detail::write_evidence_inline(output, *this);
    output << "\nEND\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PortCompatibilityEvidence::canonical_digest() const {
    return digest_of(*this);
}

core::Result<PortCompatibilityEvidence> PortCompatibilityEvidence::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxCompatibilityEvidenceBytes) {
        return core::Result<PortCompatibilityEvidence>::failure(detail::parse_error(
            "compatibility evidence text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_unsigned(input, version)) {
        return core::Result<PortCompatibilityEvidence>::failure(detail::parse_error(
            "compatibility evidence framing is invalid"));
    }
    if (magic != kEvidenceMagic) {
        return core::Result<PortCompatibilityEvidence>::failure(detail::parse_error(
            "compatibility evidence framing is invalid"));
    }
    if (version != kPortCompatibilityEvidenceVersion) {
        return core::Result<PortCompatibilityEvidence>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "compatibility evidence schema version is not supported"));
    }
    PortCompatibilityEvidence evidence;
    if (!detail::require_token(input, "EVIDENCE") ||
        !detail::read_evidence_inline(input, evidence) ||
        !detail::require_token(input, "END")) {
        return core::Result<PortCompatibilityEvidence>::failure(detail::parse_error(
            "compatibility evidence record is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PortCompatibilityEvidence>::failure(detail::parse_error(
            "compatibility evidence contains trailing unbound data"));
    }
    if (auto result = evidence.validate(); !result) {
        return core::Result<PortCompatibilityEvidence>::failure(result.error());
    }
    return core::Result<PortCompatibilityEvidence>::success(std::move(evidence));
}

core::Result<void> ConnectionPort::validate() const {
    if (!id) {
        return core::Result<void>::failure(
            detail::invalid_field("connection port identity is missing"));
    }
    if (!owner) {
        return core::Result<void>::failure(
            detail::invalid_field("connection port owner identity is missing"));
    }
    if (auto result = authoring.validate(); !result) return result;
    if (auto result = local_frame.validate(); !result) return result;
    if (compatibility_tags.size() > kMaxCompatibilityTagsPerPort ||
        !sorted_unique(compatibility_tags)) {
        return core::Result<void>::failure(detail::validation_error(
            "connection port compatibility tags must be bounded, sorted, and unique"));
    }
    for (const auto& tag : compatibility_tags) {
        if (auto result = tag.validate(); !result) return result;
    }
    return capability_contract.validate();
}

std::string ConnectionPort::serialize() const {
    std::ostringstream output;
    output << kPortMagic << ' ' << kConnectionPortVersion << '\n';
    detail::write_port_record(output, *this);
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ConnectionPort::canonical_digest() const {
    return digest_of(*this);
}

core::Result<ConnectionPort> ConnectionPort::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxConnectionPortBytes) {
        return core::Result<ConnectionPort>::failure(detail::parse_error(
            "connection port text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_unsigned(input, version)) {
        return core::Result<ConnectionPort>::failure(
            detail::parse_error("connection port framing is invalid"));
    }
    if (magic != kPortMagic) {
        return core::Result<ConnectionPort>::failure(
            detail::parse_error("connection port framing is invalid"));
    }
    if (version != kConnectionPortVersion) {
        return core::Result<ConnectionPort>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "connection port schema version is not supported"));
    }
    ConnectionPort port;
    if (!detail::read_port_record(input, port) || !detail::require_token(input, "END")) {
        return core::Result<ConnectionPort>::failure(
            detail::parse_error("connection port record is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ConnectionPort>::failure(
            detail::parse_error("connection port contains trailing unbound data"));
    }
    if (auto result = port.validate(); !result) {
        return core::Result<ConnectionPort>::failure(result.error());
    }
    return core::Result<ConnectionPort>::success(std::move(port));
}

core::Result<PortCompatibilityEvidence> check_port_compatibility(
    const ConnectionPort& first,
    const ConnectionPort& second) {
    if (auto result = first.validate(); !result) {
        return core::Result<PortCompatibilityEvidence>::failure(
            result.error().with_context("first connection port"));
    }
    if (auto result = second.validate(); !result) {
        return core::Result<PortCompatibilityEvidence>::failure(
            result.error().with_context("second connection port"));
    }

    PortCompatibilityEvidence evidence;
    std::set_intersection(
        first.compatibility_tags.begin(), first.compatibility_tags.end(),
        second.compatibility_tags.begin(), second.compatibility_tags.end(),
        std::back_inserter(evidence.shared_tags));
    if (evidence.shared_tags.empty()) {
        return core::Result<PortCompatibilityEvidence>::failure(
            detail::validation_error("connection ports share no compatibility tag"));
    }

    for (const auto& requirement : first.capability_contract.required) {
        if (std::binary_search(
                second.capability_contract.offered.begin(),
                second.capability_contract.offered.end(),
                requirement)) {
            evidence.first_satisfied_requirements.push_back(requirement);
        }
    }
    if (evidence.first_satisfied_requirements.size() != first.capability_contract.required.size()) {
        return core::Result<PortCompatibilityEvidence>::failure(
            detail::validation_error(
                "first connection port requires capabilities the second port does not offer"));
    }
    for (const auto& requirement : second.capability_contract.required) {
        if (std::binary_search(
                first.capability_contract.offered.begin(),
                first.capability_contract.offered.end(),
                requirement)) {
            evidence.second_satisfied_requirements.push_back(requirement);
        }
    }
    if (evidence.second_satisfied_requirements.size() != second.capability_contract.required.size()) {
        return core::Result<PortCompatibilityEvidence>::failure(
            detail::validation_error(
                "second connection port requires capabilities the first port does not offer"));
    }
    return core::Result<PortCompatibilityEvidence>::success(std::move(evidence));
}

} // namespace carto::connection
