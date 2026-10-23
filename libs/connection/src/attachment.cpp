#include <carto/connection/attachment.hpp>

#include "record_io.hpp"

#include <algorithm>
#include <iomanip>
#include <span>
#include <sstream>
#include <utility>

namespace carto::connection {
namespace {

constexpr std::string_view kStateMagic = "CARTOGRAPHER_PORT_ATTACHMENT_STATE";
constexpr std::string_view kPreservationMagic = "CARTOGRAPHER_ATTACHMENT_PRESERVATION";

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

bool sorted_unique(const std::vector<std::string>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

} // namespace

core::Result<void> PortAttachmentState::attach(PortId port, std::string token) {
    if (!port) {
        return core::Result<void>::failure(
            detail::invalid_field("attachment port identity is missing"));
    }
    if (!detail::safe_text(token, kMaxAttachmentTokenBytes)) {
        return core::Result<void>::failure(
            detail::invalid_field("attachment token is invalid"));
    }
    std::size_t total = 0U;
    for (const auto& [existing_port, tokens] : attachments_) {
        static_cast<void>(existing_port);
        total += tokens.size();
    }
    if (total >= kMaxAttachmentRecords) {
        return core::Result<void>::failure(detail::validation_error(
            "attachment state has reached its bounded capacity"));
    }
    auto& tokens = attachments_[port];
    if (tokens.size() >= kMaxAttachmentsPerPort) {
        return core::Result<void>::failure(detail::validation_error(
            "attachment port has reached its bounded per-port capacity"));
    }
    const auto position = std::lower_bound(tokens.begin(), tokens.end(), token);
    if (position != tokens.end() && *position == token) {
        return core::Result<void>::failure(
            detail::invalid_field("attachment token is duplicated for this port"));
    }
    tokens.insert(position, std::move(token));
    return core::Result<void>::success();
}

core::Result<void> PortAttachmentState::validate() const {
    if (attachments_.size() > kMaxAttachmentRecords) {
        return core::Result<void>::failure(detail::validation_error(
            "attachment state exceeds its bounded capacity"));
    }
    for (const auto& [port, tokens] : attachments_) {
        if (!port || tokens.empty() || tokens.size() > kMaxAttachmentsPerPort ||
            !sorted_unique(tokens)) {
            return core::Result<void>::failure(detail::validation_error(
                "attachments must be non-empty, bounded, sorted, and unique per port"));
        }
        if (!std::all_of(tokens.begin(), tokens.end(), [](const std::string& token) {
                return detail::safe_text(token, kMaxAttachmentTokenBytes);
            })) {
            return core::Result<void>::failure(
                detail::invalid_field("attachment token is invalid"));
        }
    }
    return core::Result<void>::success();
}

std::string PortAttachmentState::serialize() const {
    std::ostringstream output;
    output << kStateMagic << ' ' << kPortAttachmentStateVersion << '\n';
    output << "ATTACHMENTS " << attachments_.size() << '\n';
    for (const auto& [port, tokens] : attachments_) {
        output << "ATTACHMENT " << port.value << ' ' << tokens.size();
        for (const auto& token : tokens) detail::write_quoted(output, token);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PortAttachmentState::canonical_digest() const {
    return digest_of(*this);
}

core::Result<PortAttachmentState> PortAttachmentState::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxPortAttachmentStateBytes) {
        return core::Result<PortAttachmentState>::failure(detail::parse_error(
            "attachment state text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_unsigned(input, version)) {
        return core::Result<PortAttachmentState>::failure(
            detail::parse_error("attachment state framing is invalid"));
    }
    if (magic != kStateMagic) {
        return core::Result<PortAttachmentState>::failure(
            detail::parse_error("attachment state framing is invalid"));
    }
    if (version != kPortAttachmentStateVersion) {
        return core::Result<PortAttachmentState>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "attachment state schema version is not supported"));
    }

    std::size_t record_count = 0U;
    if (!detail::require_token(input, "ATTACHMENTS") ||
        !detail::read_count(input, kMaxAttachmentRecords, record_count)) {
        return core::Result<PortAttachmentState>::failure(
            detail::parse_error("attachment state header is invalid"));
    }
    PortAttachmentState state;
    for (std::size_t index = 0U; index < record_count; ++index) {
        PortId port;
        std::size_t token_count = 0U;
        if (!detail::require_token(input, "ATTACHMENT") ||
            !detail::read_unsigned(input, port.value) ||
            !detail::read_count(input, kMaxAttachmentsPerPort, token_count)) {
            return core::Result<PortAttachmentState>::failure(
                detail::parse_error("attachment record is malformed"));
        }
        std::vector<std::string> tokens;
        tokens.reserve(token_count);
        for (std::size_t token_index = 0U; token_index < token_count; ++token_index) {
            std::string token;
            if (!detail::read_quoted(input, token)) {
                return core::Result<PortAttachmentState>::failure(
                    detail::parse_error("attachment token is malformed"));
            }
            tokens.push_back(std::move(token));
        }
        if (!port || !state.attachments_.emplace(port, std::move(tokens)).second) {
            return core::Result<PortAttachmentState>::failure(
                detail::parse_error("attachment port identity is missing or duplicated"));
        }
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<PortAttachmentState>::failure(
            detail::parse_error("attachment state is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PortAttachmentState>::failure(
            detail::parse_error("attachment state contains trailing unbound data"));
    }
    if (auto result = state.validate(); !result) {
        return core::Result<PortAttachmentState>::failure(result.error());
    }
    return core::Result<PortAttachmentState>::success(std::move(state));
}

core::Result<void> AttachmentPreservationReceipt::validate() const {
    if (previous_state_digest.is_zero() || surviving_state_digest.is_zero() ||
        reimported_graph_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid_field(
            "preservation receipt digests must be bound and non-zero"));
    }
    if (preserved_ports.size() > kMaxAttachmentRecords ||
        dropped_ports.size() > kMaxAttachmentRecords ||
        !std::is_sorted(preserved_ports.begin(), preserved_ports.end()) ||
        std::adjacent_find(preserved_ports.begin(), preserved_ports.end()) !=
            preserved_ports.end() ||
        !std::is_sorted(dropped_ports.begin(), dropped_ports.end()) ||
        std::adjacent_find(dropped_ports.begin(), dropped_ports.end()) != dropped_ports.end()) {
        return core::Result<void>::failure(detail::validation_error(
            "preservation port lists must be bounded, sorted, and unique"));
    }
    for (const auto& port : preserved_ports) {
        if (!port) {
            return core::Result<void>::failure(
                detail::invalid_field("preservation port identity is missing"));
        }
    }
    for (const auto& port : dropped_ports) {
        if (!port) {
            return core::Result<void>::failure(
                detail::invalid_field("preservation port identity is missing"));
        }
    }
    return core::Result<void>::success();
}

std::string AttachmentPreservationReceipt::serialize() const {
    std::ostringstream output;
    output << kPreservationMagic << ' ' << kAttachmentPreservationVersion << '\n';
    output << "PREVIOUS " << previous_state_digest.hex() << '\n';
    output << "SURVIVING " << surviving_state_digest.hex() << '\n';
    output << "GRAPH " << reimported_graph_digest.hex() << '\n';
    output << "COUNTS " << preserved_attachments << ' ' << dropped_attachments << '\n';
    output << "PRESERVED " << preserved_ports.size();
    for (const auto& port : preserved_ports) output << ' ' << port.value;
    output << '\n';
    output << "DROPPED " << dropped_ports.size();
    for (const auto& port : dropped_ports) output << ' ' << port.value;
    output << "\nEND\n";
    return output.str();
}

core::Result<assets::Sha256Digest> AttachmentPreservationReceipt::canonical_digest() const {
    return digest_of(*this);
}

core::Result<AttachmentPreservationReceipt> AttachmentPreservationReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxAttachmentPreservationBytes) {
        return core::Result<AttachmentPreservationReceipt>::failure(detail::parse_error(
            "preservation receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_unsigned(input, version)) {
        return core::Result<AttachmentPreservationReceipt>::failure(
            detail::parse_error("preservation receipt framing is invalid"));
    }
    if (magic != kPreservationMagic) {
        return core::Result<AttachmentPreservationReceipt>::failure(
            detail::parse_error("preservation receipt framing is invalid"));
    }
    if (version != kAttachmentPreservationVersion) {
        return core::Result<AttachmentPreservationReceipt>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "preservation receipt schema version is not supported"));
    }

    auto read_digest = [&input](assets::Sha256Digest& digest) {
        std::string encoded;
        if (!(input >> encoded)) return false;
        const auto parsed = assets::Sha256Digest::from_hex(encoded);
        if (!parsed) return false;
        digest = parsed.value();
        return true;
    };
    auto read_port_list = [&input](std::vector<PortId>& ports) {
        std::size_t count = 0U;
        if (!detail::read_count(input, kMaxAttachmentRecords, count)) return false;
        ports.clear();
        ports.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            PortId port;
            if (!detail::read_unsigned(input, port.value)) return false;
            ports.push_back(port);
        }
        return true;
    };

    AttachmentPreservationReceipt receipt;
    if (!detail::require_token(input, "PREVIOUS") || !read_digest(receipt.previous_state_digest) ||
        !detail::require_token(input, "SURVIVING") ||
        !read_digest(receipt.surviving_state_digest) ||
        !detail::require_token(input, "GRAPH") ||
        !read_digest(receipt.reimported_graph_digest) ||
        !detail::require_token(input, "COUNTS") ||
        !detail::read_unsigned(input, receipt.preserved_attachments) ||
        !detail::read_unsigned(input, receipt.dropped_attachments) ||
        !detail::require_token(input, "PRESERVED") ||
        !read_port_list(receipt.preserved_ports) ||
        !detail::require_token(input, "DROPPED") ||
        !read_port_list(receipt.dropped_ports) ||
        !detail::require_token(input, "END")) {
        return core::Result<AttachmentPreservationReceipt>::failure(
            detail::parse_error("preservation receipt record is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<AttachmentPreservationReceipt>::failure(
            detail::parse_error("preservation receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<AttachmentPreservationReceipt>::failure(result.error());
    }
    return core::Result<AttachmentPreservationReceipt>::success(std::move(receipt));
}

core::Result<AttachmentPreservationReceipt> reconcile_attachments_after_reimport(
    const PortAttachmentState& previous_state,
    const ConnectionPortGraph& reimported_graph) {
    if (auto result = previous_state.validate(); !result) {
        return core::Result<AttachmentPreservationReceipt>::failure(
            result.error().with_context("previous attachment state"));
    }
    if (auto result = reimported_graph.validate(); !result) {
        return core::Result<AttachmentPreservationReceipt>::failure(
            result.error().with_context("reimported connection port graph"));
    }
    const auto previous_digest = previous_state.canonical_digest();
    if (!previous_digest) {
        return core::Result<AttachmentPreservationReceipt>::failure(previous_digest.error());
    }
    const auto graph_digest = reimported_graph.canonical_digest();
    if (!graph_digest) {
        return core::Result<AttachmentPreservationReceipt>::failure(graph_digest.error());
    }

    PortAttachmentState surviving;
    AttachmentPreservationReceipt receipt;
    receipt.previous_state_digest = previous_digest.value();
    receipt.reimported_graph_digest = graph_digest.value();
    for (const auto& [port, tokens] : previous_state.attachments()) {
        if (reimported_graph.ports().contains(port)) {
            for (const auto& token : tokens) {
                if (auto result = surviving.attach(port, token); !result) {
                    return core::Result<AttachmentPreservationReceipt>::failure(result.error());
                }
            }
            receipt.preserved_attachments += static_cast<std::uint64_t>(tokens.size());
            receipt.preserved_ports.push_back(port);
        } else {
            receipt.dropped_attachments += static_cast<std::uint64_t>(tokens.size());
            receipt.dropped_ports.push_back(port);
        }
    }
    const auto surviving_digest = surviving.canonical_digest();
    if (!surviving_digest) {
        return core::Result<AttachmentPreservationReceipt>::failure(surviving_digest.error());
    }
    receipt.surviving_state_digest = surviving_digest.value();
    if (auto result = receipt.validate(); !result) {
        return core::Result<AttachmentPreservationReceipt>::failure(result.error());
    }
    return core::Result<AttachmentPreservationReceipt>::success(std::move(receipt));
}

} // namespace carto::connection
