#include <carto/condition/sweep_endpoint.hpp>

#include <iomanip>
#include <locale>
#include <sstream>
#include <span>
#include <string_view>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view value) noexcept {
    if (value.empty() || value.size() > SweepEndpointPortBinding::kMaxIdentityBytes) {
        return false;
    }
    for (const char character : value) {
        const bool letter = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    return value.front() != '.' && value.front() != '_' && value.front() != '-';
}

bool valid_side(SweepEndpointSide side) noexcept {
    return side == SweepEndpointSide::start || side == SweepEndpointSide::end;
}

bool same_endpoint(
    const SweepEndpointPortBinding& first,
    const SweepEndpointPortBinding& second) noexcept {
    return first.sweep_source_identity == second.sweep_source_identity &&
        first.sweep_source_revision == second.sweep_source_revision &&
        first.side == second.side;
}

assets::Sha256Digest source_digest(const geometry::SweepSource& source) {
    const std::string encoded = source.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()});
}

core::Result<void> validate_binding_record(const SweepEndpointPortBinding& binding) {
    if (!valid_identity(binding.sweep_source_identity) ||
        binding.sweep_source_revision.value() == 0U || !valid_side(binding.side) ||
        binding.port_id.value == 0U) {
        return core::Result<void>::failure(invalid(
            "sweep endpoint binding identity, revision, side, or port is invalid"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> SweepEndpointPortBinding::validate(
    const geometry::SweepSource& source,
    const connection::ConnectionPort& port) const {
    if (auto result = source.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("sweep endpoint source"));
    }
    if (auto result = validate_binding_record(*this); !result) return result;
    if (sweep_source_identity != source.source_identity ||
        sweep_source_revision != source.source_revision) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "sweep endpoint binding does not target the supplied source revision"));
    }
    if (port.id != port_id) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "sweep endpoint binding does not target the supplied connection port"));
    }
    return port.validate();
}

core::Result<void> SweepEndpointCompatibilityEvidence::validate() const {
    if (auto result = validate_binding_record(first); !result) return result;
    if (auto result = validate_binding_record(second); !result) return result;
    if (same_endpoint(first, second) || first.port_id == second.port_id) {
        return core::Result<void>::failure(validation(
            "sweep endpoint compatibility evidence repeats one endpoint or port"));
    }
    if (first.sweep_source_identity == second.sweep_source_identity &&
        (first.sweep_source_revision != second.sweep_source_revision ||
         first_sweep_digest != second_sweep_digest)) {
        return core::Result<void>::failure(validation(
            "one sweep identity cannot bind conflicting revisions or source digests"));
    }
    if (first_sweep_digest.is_zero() || second_sweep_digest.is_zero() ||
        first_port_digest.is_zero() || second_port_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "sweep endpoint evidence requires both source and port digests"));
    }
    return port_compatibility.validate();
}

std::string SweepEndpointCompatibilityEvidence::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTOGRAPHER_SWEEP_ENDPOINT_COMPATIBILITY " << kSchemaVersion << '\n';
    output << "FIRST " << std::quoted(first.sweep_source_identity) << ' '
           << first.sweep_source_revision.value() << ' '
           << static_cast<unsigned>(first.side) << ' ' << first.port_id.value << ' '
           << first_sweep_digest.hex() << ' ' << first_port_digest.hex() << '\n';
    output << "SECOND " << std::quoted(second.sweep_source_identity) << ' '
           << second.sweep_source_revision.value() << ' '
           << static_cast<unsigned>(second.side) << ' ' << second.port_id.value << ' '
           << second_sweep_digest.hex() << ' ' << second_port_digest.hex() << '\n';
    output << "PORT_COMPATIBILITY "
           << std::quoted(port_compatibility.serialize()) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest>
SweepEndpointCompatibilityEvidence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string encoded = serialize();
    if (encoded.size() > kMaxSerializedBytes) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "sweep endpoint evidence exceeds its serialized byte bound"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()}));
}

core::Result<SweepEndpointCompatibilityEvidence> check_sweep_endpoint_compatibility(
    const geometry::SweepSource& first_source,
    const SweepEndpointPortBinding& first_binding,
    const connection::ConnectionPort& first_port,
    const geometry::SweepSource& second_source,
    const SweepEndpointPortBinding& second_binding,
    const connection::ConnectionPort& second_port) {
    if (auto result = first_binding.validate(first_source, first_port); !result) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            result.error().with_context("first sweep endpoint"));
    }
    if (auto result = second_binding.validate(second_source, second_port); !result) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            result.error().with_context("second sweep endpoint"));
    }
    if (same_endpoint(first_binding, second_binding) ||
        first_binding.port_id == second_binding.port_id) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(validation(
            "sweep endpoint join requires two distinct endpoint and port identities"));
    }
    const auto first_source_digest = source_digest(first_source);
    const auto second_source_digest = source_digest(second_source);
    if (first_source.source_identity == second_source.source_identity &&
        (first_source.source_revision != second_source.source_revision ||
         first_source_digest != second_source_digest)) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "one sweep identity was supplied with conflicting revision-bound recipes"));
    }
    const auto first_port_digest = first_port.canonical_digest();
    if (!first_port_digest) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            first_port_digest.error().with_context("first sweep endpoint port digest"));
    }
    const auto second_port_digest = second_port.canonical_digest();
    if (!second_port_digest) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            second_port_digest.error().with_context("second sweep endpoint port digest"));
    }
    const auto compatible = connection::check_port_compatibility(first_port, second_port);
    if (!compatible) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            compatible.error().with_context("sweep endpoint compatibility"));
    }

    SweepEndpointCompatibilityEvidence evidence{
        first_binding,
        second_binding,
        first_source_digest,
        second_source_digest,
        first_port_digest.value(),
        second_port_digest.value(),
        compatible.value()};
    if (auto result = evidence.validate(); !result) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            result.error().with_context("sweep endpoint compatibility evidence"));
    }
    return core::Result<SweepEndpointCompatibilityEvidence>::success(std::move(evidence));
}

} // namespace carto::condition
