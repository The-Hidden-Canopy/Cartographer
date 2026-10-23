#include <carto/physical/twin.hpp>

#include <algorithm>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kTwinMagic = "CARTOGRAPHER_PHYSICAL_TWIN";
constexpr std::string_view kReceiptMagic = "CARTOGRAPHER_PHYSICAL_TWIN_ACCEPTANCE";
constexpr double kConservationEpsilon = 1e-9;

core::Result<assets::Sha256Digest> digest_text(std::string_view canonical) {
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

} // namespace

core::Result<void> PhysicalTwinRecord::validate() const {
    if (!detail::safe_text(source_namespace, kMaxNamespaceBytes) ||
        source_revision.exhausted() || source_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "physical twin source namespace, revision, or digest is invalid"));
    }
    if (auto result = geometry.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("physical twin geometry evidence"));
    }
    if (auto result = construction.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("physical twin construction profile"));
    }
    if (auto result = mass.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("physical twin mass receipt"));
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("physical twin surface ledger"));
    }
    if (ground_pressure.has_value()) {
        if (auto result = ground_pressure->validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("physical twin ground pressure"));
        }
    }
    if (condition_states.size() > kMaxConditionStates) {
        return core::Result<void>::failure(detail::validation(
            "physical twin carries more game-owned condition state than its bound"));
    }
    for (const auto& state : condition_states) {
        if (auto result = state.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string PhysicalTwinRecord::serialize() const {
    // Canonical document form. Game-owned condition state is deliberately
    // absent: nothing in this byte stream may depend on runtime truth.
    std::ostringstream output;
    output << kTwinMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' ';
    detail::write_revision(output, source_revision) << ' ';
    detail::write_digest(output, source_digest) << '\n';
    output << "GEOMETRY_EVIDENCE\n";
    detail::encode_evidence(output, geometry);
    output << "CONSTRUCTION\n";
    detail::encode_construction(output, construction);
    output << "MASS\n";
    detail::encode_mass(output, mass);
    output << "LEDGER\n";
    detail::encode_ledger(output, ledger);
    output << "GROUND_PRESSURE " << (ground_pressure.has_value() ? 1 : 0) << '\n';
    if (ground_pressure.has_value()) {
        detail::encode_ground_pressure(output, ground_pressure.value());
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PhysicalTwinRecord::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return digest_text(serialize());
}

core::Result<PhysicalTwinRecord> PhysicalTwinRecord::deserialize(std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "physical twin");
        !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::read_frame(input, kTwinMagic, kSchemaVersion); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    PhysicalTwinRecord record;
    std::string source_digest;
    if (!detail::require_token(input, "SOURCE") ||
        !(input >> std::quoted(record.source_namespace)) ||
        !detail::read_revision(input, record.source_revision) ||
        !(input >> source_digest)) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin source record is malformed"));
    }
    const auto parsed_source_digest = assets::Sha256Digest::from_hex(source_digest);
    if (!parsed_source_digest) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin source digest is malformed"));
    }
    record.source_digest = parsed_source_digest.value();
    if (!detail::require_token(input, "GEOMETRY_EVIDENCE")) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin geometry evidence section is malformed"));
    }
    if (auto result = detail::decode_evidence(input, record.geometry); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    if (!detail::require_token(input, "CONSTRUCTION")) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin construction section is malformed"));
    }
    if (auto result = detail::decode_construction(input, record.construction); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    if (!detail::require_token(input, "MASS")) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin mass section is malformed"));
    }
    if (auto result = detail::decode_mass(input, record.mass); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    if (!detail::require_token(input, "LEDGER")) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin ledger section is malformed"));
    }
    if (auto result = detail::decode_ledger(input, record.ledger); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    unsigned has_pressure = 0U;
    if (!detail::require_token(input, "GROUND_PRESSURE") ||
        !detail::read_integer(input, has_pressure) || has_pressure > 1U) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin ground pressure section is malformed"));
    }
    if (has_pressure != 0U) {
        record.ground_pressure.emplace();
        if (auto result =
                detail::decode_ground_pressure(input, record.ground_pressure.value());
            !result) {
            return core::Result<PhysicalTwinRecord>::failure(result.error());
        }
    }
    if (auto result = detail::read_end(input, kTwinMagic); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PhysicalTwinRecord>::failure(detail::parse_error(
            "physical twin contains trailing unbound data"));
    }
    // Condition state never round-trips through the canonical document.
    record.condition_states.clear();
    if (auto result = record.validate(); !result) {
        return core::Result<PhysicalTwinRecord>::failure(result.error());
    }
    return core::Result<PhysicalTwinRecord>::success(std::move(record));
}

core::Result<void> PhysicalTwinAcceptanceReceipt::validate() const {
    if (!detail::safe_text(source_namespace, PhysicalTwinRecord::kMaxNamespaceBytes) ||
        source_revision.exhausted() || source_digest.is_zero() || twin_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "acceptance receipt source identity is invalid"));
    }
    if (!std::isfinite(prior_total_mass_kg) || !std::isfinite(candidate_total_mass_kg) ||
        !std::isfinite(prior_volume_m3) || !std::isfinite(candidate_volume_m3) ||
        prior_total_mass_kg < 0.0 || candidate_total_mass_kg <= 0.0 ||
        prior_volume_m3 < 0.0 || candidate_volume_m3 <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "acceptance receipt conservation values are invalid"));
    }
    if (!first_acceptance && !prior_twin_digest.has_value()) {
        return core::Result<void>::failure(detail::validation(
            "acceptance receipt for a repeat acceptance must bind the prior twin"));
    }
    if (first_acceptance &&
        (prior_twin_digest.has_value() || prior_total_mass_kg != 0.0 ||
         prior_volume_m3 != 0.0)) {
        return core::Result<void>::failure(detail::validation(
            "first acceptance receipt must not bind prior state"));
    }
    if (violations.size() > kMaxViolations ||
        !std::all_of(violations.begin(), violations.end(), [](const std::string& item) {
            return detail::safe_text(item, 512U);
        })) {
        return core::Result<void>::failure(detail::invalid(
            "acceptance receipt violations are invalid"));
    }
    if (accepted &&
        (!evidence_digests_bound || !mass_conservation_observed ||
         !volume_conservation_observed || !violations.empty())) {
        return core::Result<void>::failure(detail::validation(
            "acceptance receipt cannot be accepted with failed checks"));
    }
    return core::Result<void>::success();
}

std::string PhysicalTwinAcceptanceReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' ';
    detail::write_revision(output, source_revision) << ' ';
    detail::write_digest(output, source_digest) << ' ';
    detail::write_digest(output, twin_digest) << '\n';
    output << "ACCEPTANCE " << (first_acceptance ? 1 : 0) << ' ';
    detail::write_digest(output, prior_twin_digest) << ' '
           << std::setprecision(17) << prior_total_mass_kg << ' '
           << candidate_total_mass_kg << ' ' << prior_volume_m3 << ' '
           << candidate_volume_m3 << '\n';
    output << "CHECKS " << (evidence_digests_bound ? 1 : 0) << ' '
           << (mass_conservation_observed ? 1 : 0) << ' '
           << (volume_conservation_observed ? 1 : 0) << ' '
           << game_owned_condition_records << ' ' << violations.size() << '\n';
    for (const auto& violation : violations) {
        output << "VIOLATION " << std::quoted(violation) << '\n';
    }
    output << "VERDICT " << (accepted ? 1 : 0) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PhysicalTwinAcceptanceReceipt::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return digest_text(serialize());
}

core::Result<PhysicalTwinAcceptanceReceipt>
PhysicalTwinAcceptanceReceipt::deserialize(std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "acceptance receipt");
        !result) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::read_frame(input, kReceiptMagic, kSchemaVersion);
        !result) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(result.error());
    }
    PhysicalTwinAcceptanceReceipt receipt;
    std::string source_digest;
    if (!detail::require_token(input, "SOURCE") ||
        !(input >> std::quoted(receipt.source_namespace)) ||
        !detail::read_revision(input, receipt.source_revision) ||
        !(input >> source_digest)) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt source record is malformed"));
    }
    const auto parsed_source = assets::Sha256Digest::from_hex(source_digest);
    if (!parsed_source) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt source digest is malformed"));
    }
    receipt.source_digest = parsed_source.value();
    if (!detail::read_digest(input, receipt.twin_digest)) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt twin digest is malformed"));
    }
    unsigned first = 0U;
    std::size_t violation_count = 0U;
    unsigned evidence_bound = 0U;
    unsigned mass_conserved = 0U;
    unsigned volume_conserved = 0U;
    unsigned verdict = 0U;
    if (!detail::require_token(input, "ACCEPTANCE") ||
        !detail::read_integer(input, first) || first > 1U ||
        !detail::read_digest(input, receipt.prior_twin_digest) ||
        !detail::read_double(input, receipt.prior_total_mass_kg) ||
        !detail::read_double(input, receipt.candidate_total_mass_kg) ||
        !detail::read_double(input, receipt.prior_volume_m3) ||
        !detail::read_double(input, receipt.candidate_volume_m3)) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt conservation record is malformed"));
    }
    receipt.first_acceptance = first != 0U;
    if (!detail::require_token(input, "CHECKS") ||
        !detail::read_integer(input, evidence_bound) || evidence_bound > 1U ||
        !detail::read_integer(input, mass_conserved) || mass_conserved > 1U ||
        !detail::read_integer(input, volume_conserved) || volume_conserved > 1U ||
        !detail::read_integer(input, receipt.game_owned_condition_records) ||
        !detail::read_count(input, kMaxViolations, violation_count)) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt checks record is malformed"));
    }
    receipt.evidence_digests_bound = evidence_bound != 0U;
    receipt.mass_conservation_observed = mass_conserved != 0U;
    receipt.volume_conservation_observed = volume_conserved != 0U;
    receipt.violations.reserve(violation_count);
    for (std::size_t index = 0U; index < violation_count; ++index) {
        std::string violation;
        if (!detail::require_token(input, "VIOLATION") ||
            !(input >> std::quoted(violation))) {
            return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
                detail::parse_error("acceptance receipt violation is malformed"));
        }
        receipt.violations.push_back(std::move(violation));
    }
    if (!detail::require_token(input, "VERDICT") ||
        !detail::read_integer(input, verdict) || verdict > 1U) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt verdict is malformed"));
    }
    receipt.accepted = verdict != 0U;
    if (auto result = detail::read_end(input, kReceiptMagic); !result) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            detail::parse_error("acceptance receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(result.error());
    }
    return core::Result<PhysicalTwinAcceptanceReceipt>::success(std::move(receipt));
}

core::Result<PhysicalTwinAcceptanceReceipt> evaluate_physical_twin_acceptance(
    const PhysicalTwinRecord& candidate,
    const PhysicalTwinRecord* prior) {
    if (auto result = candidate.validate(); !result) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            result.error().with_context("physical twin acceptance candidate"));
    }
    if (prior != nullptr) {
        if (auto result = prior->validate(); !result) {
            return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
                result.error().with_context("physical twin acceptance prior"));
        }
    }

    PhysicalTwinAcceptanceReceipt receipt;
    receipt.source_namespace = candidate.source_namespace;
    receipt.source_revision = candidate.source_revision;
    receipt.source_digest = candidate.source_digest;
    receipt.first_acceptance = prior == nullptr;
    receipt.candidate_total_mass_kg = candidate.mass.total_mass_kg;
    receipt.candidate_volume_m3 = candidate.mass.total_volume_m3;
    receipt.game_owned_condition_records =
        static_cast<std::uint64_t>(candidate.condition_states.size());

    const auto twin_digest = candidate.canonical_digest();
    if (!twin_digest) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(twin_digest.error());
    }
    receipt.twin_digest = twin_digest.value();

    // Derived values must bind the exact evidence digests of the inputs they
    // claim to derive from. Anything else is stale data and fails closed.
    const auto geometry_digest = candidate.geometry.canonical_digest();
    if (!geometry_digest) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(geometry_digest.error());
    }
    const auto construction_digest = candidate.construction.canonical_digest();
    if (!construction_digest) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
            construction_digest.error());
    }
    receipt.evidence_digests_bound =
        candidate.mass.geometry_evidence_digest == geometry_digest.value() &&
        candidate.mass.construction_profile_digest == construction_digest.value();
    if (!receipt.evidence_digests_bound) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(detail::stale(
            "derived mass binds evidence digests that do not match the twin inputs"));
    }
    if (candidate.ground_pressure.has_value() &&
        candidate.ground_pressure->mass_receipt_digest !=
            candidate.mass.canonical_digest().value()) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(detail::stale(
            "ground pressure binds a mass receipt digest that does not match the twin"));
    }

    if (prior != nullptr) {
        const auto prior_digest = prior->canonical_digest();
        if (!prior_digest) {
            return core::Result<PhysicalTwinAcceptanceReceipt>::failure(
                prior_digest.error());
        }
        receipt.prior_twin_digest = prior_digest.value();
        receipt.prior_total_mass_kg = prior->mass.total_mass_kg;
        receipt.prior_volume_m3 = prior->mass.total_volume_m3;

        // Conservation: with no authored change (identical source digest),
        // derived mass and enclosed volume must not increase.
        const bool authored_change = prior->source_digest != candidate.source_digest;
        if (!authored_change) {
            receipt.mass_conservation_observed =
                candidate.mass.total_mass_kg <=
                prior->mass.total_mass_kg *
                    (1.0 + kConservationEpsilon);
            receipt.volume_conservation_observed =
                candidate.mass.total_volume_m3 <=
                prior->mass.total_volume_m3 *
                    (1.0 + kConservationEpsilon);
            if (!receipt.mass_conservation_observed) {
                receipt.violations.push_back(
                    "total mass increased without an authored change");
            }
            if (!receipt.volume_conservation_observed) {
                receipt.violations.push_back(
                    "enclosed volume increased without an authored change");
            }
        }
    }

    receipt.accepted = receipt.violations.empty();
    if (auto result = receipt.validate(); !result) {
        return core::Result<PhysicalTwinAcceptanceReceipt>::failure(result.error());
    }
    return core::Result<PhysicalTwinAcceptanceReceipt>::success(std::move(receipt));
}

} // namespace carto::physical
