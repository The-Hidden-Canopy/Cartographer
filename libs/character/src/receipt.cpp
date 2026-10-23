#include <carto/character/receipt.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <span>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

} // namespace

std::string_view gate_status_name(GateStatus status) noexcept {
    switch (status) {
    case GateStatus::pass: return "pass";
    case GateStatus::fail: return "fail";
    case GateStatus::hold: return "hold";
    case GateStatus::not_run: return "not-run";
    }
    return "unknown";
}

core::Result<void> CharacterProductReceipt::validate() const {
    if (!safe_text(producer_build, kMaxIdentityBytes) ||
        !safe_text(source_namespace, kMaxNamespaceBytes) ||
        source_revision.value() == 0U || source_digest.is_zero() ||
        recipe_digest.is_zero() || format_schema_major == 0U ||
        !safe_text(backend_identity, kMaxIdentityBytes) ||
        !safe_text(precision_identity, kMaxIdentityBytes) ||
        output_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "character product receipt identity, provenance, recipe, schema, backend, or output is invalid"));
    }
    return core::Result<void>::success();
}

std::string CharacterProductReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTO_CHARACTER_PRODUCT_RECEIPT 1\n";
    output << "PRODUCER " << std::quoted(producer_build) << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << source_revision.value() << ' ' << source_digest.hex() << '\n';
    output << "RECIPE " << recipe_digest.hex() << '\n';
    output << "SCHEMA " << format_schema_major << ' ' << format_schema_minor << '\n';
    output << "BACKEND " << std::quoted(backend_identity) << ' '
           << std::quoted(precision_identity) << '\n';
    output << "OUTPUT " << output_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CharacterProductReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<void> CharacterPromotionReceipt::validate() const {
    if (!safe_text(gate_ledger_ref, kMaxLedgerRefBytes) ||
        gate_statuses.size() > kMaxGates || gate_statuses.empty() ||
        !safe_text(signer, kMaxSignerBytes) || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "character promotion receipt ledger, gates, signer, or digest is invalid"));
    }
    for (const auto& [gate, status] : gate_statuses) {
        if (!safe_text(gate, kMaxGateIdBytes)) {
            return core::Result<void>::failure(invalid(
                "character promotion gate identity is invalid"));
        }
        switch (status) {
        case GateStatus::pass:
        case GateStatus::fail:
        case GateStatus::hold:
        case GateStatus::not_run:
            break;
        default:
            return core::Result<void>::failure(invalid(
                "character promotion gate status is invalid"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
