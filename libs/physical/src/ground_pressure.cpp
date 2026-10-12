#include <carto/physical/ground_pressure.hpp>

#include <iomanip>
#include <span>
#include <sstream>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PHYSICAL_GROUND_PRESSURE_EVIDENCE";

std::ostream& write_bounds(std::ostream& output, const core::Bounds3d& bounds) {
    output << bounds.minimum.x << ' ' << bounds.minimum.y << ' ' << bounds.minimum.z
           << ' ' << bounds.maximum.x << ' ' << bounds.maximum.y << ' '
           << bounds.maximum.z;
    return output;
}

bool read_bounds(std::istream& input, core::Bounds3d& bounds) {
    return detail::read_double(input, bounds.minimum.x) &&
        detail::read_double(input, bounds.minimum.y) &&
        detail::read_double(input, bounds.minimum.z) &&
        detail::read_double(input, bounds.maximum.x) &&
        detail::read_double(input, bounds.maximum.y) &&
        detail::read_double(input, bounds.maximum.z);
}

} // namespace

core::Result<void> GroundPressureEvidence::validate() const {
    if (mass_receipt_digest.is_zero() || source_revision.exhausted() ||
        !contact_patch.valid() || !detail::finite_positive(contact_patch_area) ||
        !detail::finite_positive(supported_mass_kg) ||
        !detail::finite_positive(weight_newtons) ||
        !detail::finite_positive(mean_pressure_pa) ||
        !center_of_pressure.finite() ||
        !detail::safe_text(pressure_model, 128U) ||
        !detail::safe_text(algorithm_version, 128U)) {
        return core::Result<void>::failure(detail::invalid(
            "ground pressure evidence identity, patch, or values are invalid"));
    }
    if (center_of_pressure.x < contact_patch.minimum.x ||
        center_of_pressure.x > contact_patch.maximum.x ||
        center_of_pressure.z < contact_patch.minimum.z ||
        center_of_pressure.z > contact_patch.maximum.z) {
        return core::Result<void>::failure(detail::validation(
            "center of pressure falls outside the contact patch"));
    }
    const double expected_area =
        (contact_patch.maximum.x - contact_patch.minimum.x) *
        (contact_patch.maximum.z - contact_patch.minimum.z);
    if (std::abs(expected_area - contact_patch_area) >
        1e-9 * (contact_patch_area > 0.0 ? contact_patch_area : 1.0)) {
        return core::Result<void>::failure(detail::validation(
            "contact patch area does not match its bounds"));
    }
    const double expected_weight = supported_mass_kg * kStandardGravityMetersPerSecond2;
    if (std::abs(expected_weight - weight_newtons) >
        1e-9 * (weight_newtons > 0.0 ? weight_newtons : 1.0)) {
        return core::Result<void>::failure(detail::validation(
            "ground pressure weight does not match its mass"));
    }
    const double expected_pressure = weight_newtons / contact_patch_area;
    if (std::abs(expected_pressure - mean_pressure_pa) >
        1e-9 * (mean_pressure_pa > 0.0 ? mean_pressure_pa : 1.0)) {
        return core::Result<void>::failure(detail::validation(
            "ground pressure value does not match its weight and patch"));
    }
    return core::Result<void>::success();
}

std::string GroundPressureEvidence::serialize() const {
    std::ostringstream output;
    detail::encode_ground_pressure(output, *this);
    return output.str();
}

core::Result<assets::Sha256Digest> GroundPressureEvidence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<GroundPressureEvidence> GroundPressureEvidence::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "ground pressure evidence");
        !result) {
        return core::Result<GroundPressureEvidence>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    GroundPressureEvidence evidence;
    if (auto result = detail::decode_ground_pressure(input, evidence); !result) {
        return core::Result<GroundPressureEvidence>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<GroundPressureEvidence>::failure(detail::parse_error(
            "ground pressure evidence contains trailing unbound data"));
    }
    if (auto result = evidence.validate(); !result) {
        return core::Result<GroundPressureEvidence>::failure(result.error());
    }
    return core::Result<GroundPressureEvidence>::success(std::move(evidence));
}

namespace detail {

void encode_ground_pressure(
    std::ostream& output, const GroundPressureEvidence& evidence) {
    output << kMagic << ' ' << GroundPressureEvidence::kSchemaVersion << '\n';
    output << "BINDS ";
    write_digest(output, evidence.mass_receipt_digest) << ' ';
    write_revision(output, evidence.source_revision) << '\n';
    output << std::setprecision(17);
    output << "PATCH ";
    write_bounds(output, evidence.contact_patch) << '\n';
    output << "PRESSURE " << evidence.contact_patch_area << ' '
           << evidence.supported_mass_kg << ' ' << evidence.weight_newtons << ' '
           << evidence.mean_pressure_pa << '\n';
    output << "CENTER_OF_PRESSURE " << evidence.center_of_pressure.x << ' '
           << evidence.center_of_pressure.y << ' ' << evidence.center_of_pressure.z
           << '\n';
    output << "ALGORITHM " << std::quoted(evidence.pressure_model) << ' '
           << std::quoted(evidence.algorithm_version) << '\n';
    output << "END\n";
}

core::Result<void> decode_ground_pressure(
    std::istream& input, GroundPressureEvidence& evidence) {
    if (auto result = read_frame(input, kMagic, GroundPressureEvidence::kSchemaVersion);
        !result) {
        return result;
    }
    if (!require_token(input, "BINDS") ||
        !read_digest(input, evidence.mass_receipt_digest) ||
        !read_revision(input, evidence.source_revision)) {
        return core::Result<void>::failure(parse_error(
            "ground pressure evidence binding record is malformed"));
    }
    if (!require_token(input, "PATCH") || !read_bounds(input, evidence.contact_patch)) {
        return core::Result<void>::failure(parse_error(
            "ground pressure evidence patch record is malformed"));
    }
    if (!require_token(input, "PRESSURE") ||
        !read_double(input, evidence.contact_patch_area) ||
        !read_double(input, evidence.supported_mass_kg) ||
        !read_double(input, evidence.weight_newtons) ||
        !read_double(input, evidence.mean_pressure_pa)) {
        return core::Result<void>::failure(parse_error(
            "ground pressure evidence values are malformed"));
    }
    if (!require_token(input, "CENTER_OF_PRESSURE") ||
        !read_double(input, evidence.center_of_pressure.x) ||
        !read_double(input, evidence.center_of_pressure.y) ||
        !read_double(input, evidence.center_of_pressure.z)) {
        return core::Result<void>::failure(parse_error(
            "ground pressure evidence center of pressure is malformed"));
    }
    if (!require_token(input, "ALGORITHM") ||
        !(input >> std::quoted(evidence.pressure_model)) ||
        !(input >> std::quoted(evidence.algorithm_version))) {
        return core::Result<void>::failure(parse_error(
            "ground pressure evidence algorithm record is malformed"));
    }
    return read_end(input, kMagic);
}

} // namespace detail

core::Result<GroundPressureEvidence> derive_ground_pressure(
    const MassDerivationReceipt& mass,
    core::Bounds3d contact_patch) {
    if (auto result = mass.validate(); !result) {
        return core::Result<GroundPressureEvidence>::failure(
            result.error().with_context("ground pressure mass receipt"));
    }
    if (!contact_patch.valid()) {
        return core::Result<GroundPressureEvidence>::failure(detail::invalid(
            "ground pressure requires a valid contact patch"));
    }
    const double area = (contact_patch.maximum.x - contact_patch.minimum.x) *
        (contact_patch.maximum.z - contact_patch.minimum.z);
    if (!(area > 0.0) || !std::isfinite(area)) {
        return core::Result<GroundPressureEvidence>::failure(detail::validation(
            "ground pressure contact patch is degenerate"));
    }
    const double weight = mass.total_mass_kg * kStandardGravityMetersPerSecond2;

    GroundPressureEvidence evidence;
    evidence.mass_receipt_digest = mass.canonical_digest().value();
    evidence.source_revision = mass.source_revision;
    evidence.contact_patch = contact_patch;
    evidence.contact_patch_area = area;
    evidence.supported_mass_kg = mass.total_mass_kg;
    evidence.weight_newtons = weight;
    evidence.mean_pressure_pa = weight / area;
    evidence.center_of_pressure = {
        mass.center_of_mass.x,
        contact_patch.maximum.y,
        mass.center_of_mass.z,
    };

    if (auto result = evidence.validate(); !result) {
        return core::Result<GroundPressureEvidence>::failure(result.error());
    }
    return core::Result<GroundPressureEvidence>::success(std::move(evidence));
}

} // namespace carto::physical
