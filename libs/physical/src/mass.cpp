#include <carto/physical/mass.hpp>

#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PHYSICAL_MASS_DERIVATION";

core::Result<double> require_density(
    const PhysicalMaterialIntent& intent, std::string_view role) {
    if (!intent.density_kg_per_m3.has_value()) {
        return core::Result<double>::failure(detail::validation(
            std::string("mass derivation requires an authored density for the ") +
            std::string(role) + " material intent"));
    }
    return core::Result<double>::success(intent.density_kg_per_m3.value());
}

} // namespace

core::Result<void> MassDerivationReceipt::validate() const {
    if (geometry_evidence_digest.is_zero() || construction_profile_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "mass derivation receipt does not bind its evidence digests"));
    }
    if (!detail::safe_text(algorithm_version, 128U) ||
        !detail::safe_text(distribution_model, 128U) || source_revision.exhausted()) {
        return core::Result<void>::failure(detail::invalid(
            "mass derivation receipt algorithm, model, or revision is invalid"));
    }
    if (!detail::finite_positive(total_mass_kg) ||
        !detail::finite_positive(total_volume_m3) || !center_of_mass.finite() ||
        !inertia_kg_m2.finite() || !inertia_kg_m2.positive_diagonal()) {
        return core::Result<void>::failure(detail::validation(
            "mass derivation receipt values are physically invalid"));
    }
    return core::Result<void>::success();
}

std::string MassDerivationReceipt::serialize() const {
    std::ostringstream output;
    detail::encode_mass(output, *this);
    return output.str();
}

core::Result<assets::Sha256Digest> MassDerivationReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<MassDerivationReceipt> MassDerivationReceipt::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "mass receipt");
        !result) {
        return core::Result<MassDerivationReceipt>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    MassDerivationReceipt receipt;
    if (auto result = detail::decode_mass(input, receipt); !result) {
        return core::Result<MassDerivationReceipt>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MassDerivationReceipt>::failure(detail::parse_error(
            "mass derivation receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<MassDerivationReceipt>::failure(result.error());
    }
    return core::Result<MassDerivationReceipt>::success(std::move(receipt));
}

namespace detail {

void encode_mass(std::ostream& output, const MassDerivationReceipt& receipt) {
    output << kMagic << ' ' << MassDerivationReceipt::kSchemaVersion << '\n';
    output << "BINDS ";
    write_digest(output, receipt.geometry_evidence_digest) << ' ';
    write_digest(output, receipt.construction_profile_digest) << '\n';
    output << "ALGORITHM " << std::quoted(receipt.algorithm_version) << ' '
           << std::quoted(receipt.distribution_model) << ' ';
    write_revision(output, receipt.source_revision) << '\n';
    output << std::setprecision(17);
    output << "MASS " << receipt.total_mass_kg << ' ' << receipt.total_volume_m3 << '\n';
    output << "CENTER_OF_MASS " << receipt.center_of_mass.x << ' '
           << receipt.center_of_mass.y << ' ' << receipt.center_of_mass.z << '\n';
    output << "INERTIA ";
    write_tensor(output, receipt.inertia_kg_m2) << '\n';
    output << "END\n";
}

core::Result<void> decode_mass(std::istream& input, MassDerivationReceipt& receipt) {
    if (auto result = read_frame(input, kMagic, MassDerivationReceipt::kSchemaVersion);
        !result) {
        return result;
    }
    if (!require_token(input, "BINDS") ||
        !read_digest(input, receipt.geometry_evidence_digest) ||
        !read_digest(input, receipt.construction_profile_digest)) {
        return core::Result<void>::failure(parse_error(
            "mass derivation receipt binding record is malformed"));
    }
    if (!require_token(input, "ALGORITHM") ||
        !(input >> std::quoted(receipt.algorithm_version)) ||
        !(input >> std::quoted(receipt.distribution_model)) ||
        !read_revision(input, receipt.source_revision)) {
        return core::Result<void>::failure(parse_error(
            "mass derivation receipt algorithm record is malformed"));
    }
    if (!require_token(input, "MASS") || !read_double(input, receipt.total_mass_kg) ||
        !read_double(input, receipt.total_volume_m3)) {
        return core::Result<void>::failure(parse_error(
            "mass derivation receipt mass record is malformed"));
    }
    if (!require_token(input, "CENTER_OF_MASS") ||
        !read_double(input, receipt.center_of_mass.x) ||
        !read_double(input, receipt.center_of_mass.y) ||
        !read_double(input, receipt.center_of_mass.z)) {
        return core::Result<void>::failure(parse_error(
            "mass derivation receipt center-of-mass record is malformed"));
    }
    if (!require_token(input, "INERTIA") || !read_tensor(input, receipt.inertia_kg_m2)) {
        return core::Result<void>::failure(parse_error(
            "mass derivation receipt inertia record is malformed"));
    }
    return read_end(input, kMagic);
}

} // namespace detail

core::Result<MassDerivationReceipt> derive_mass(
    const GeometryEvidence& geometry,
    const ConstructionProfile& construction) {
    if (auto result = geometry.validate(); !result) {
        return core::Result<MassDerivationReceipt>::failure(
            result.error().with_context("mass derivation geometry evidence"));
    }
    if (auto result = construction.validate(); !result) {
        return core::Result<MassDerivationReceipt>::failure(
            result.error().with_context("mass derivation construction profile"));
    }

    double total_mass = 0.0;
    switch (construction.kind) {
    case ConstructionKind::solid: {
        const auto density =
            require_density(construction.primary_material, "primary");
        if (!density) return core::Result<MassDerivationReceipt>::failure(density.error());
        total_mass = density.value() * geometry.volume;
        break;
    }
    case ConstructionKind::shell: {
        const auto density =
            require_density(construction.primary_material, "wall");
        if (!density) return core::Result<MassDerivationReceipt>::failure(density.error());
        total_mass = density.value() * geometry.surface_area *
            construction.wall_thickness.value();
        break;
    }
    case ConstructionKind::layered: {
        for (std::size_t index = 0U; index < construction.layers.size(); ++index) {
            const auto density = require_density(
                construction.layers[index].material, "layer");
            if (!density) {
                return core::Result<MassDerivationReceipt>::failure(density.error());
            }
            total_mass += density.value() * geometry.surface_area *
                construction.layers[index].nominal_thickness;
        }
        break;
    }
    case ConstructionKind::filled: {
        const auto wall_density =
            require_density(construction.primary_material, "wall");
        if (!wall_density) {
            return core::Result<MassDerivationReceipt>::failure(wall_density.error());
        }
        const auto fill_density =
            require_density(construction.fill_material.value(), "fill");
        if (!fill_density) {
            return core::Result<MassDerivationReceipt>::failure(fill_density.error());
        }
        const double wall_volume =
            geometry.surface_area * construction.wall_thickness.value();
        if (wall_volume >= geometry.volume) {
            return core::Result<MassDerivationReceipt>::failure(detail::validation(
                "filled construction wall exceeds the enclosed mesh volume"));
        }
        total_mass = wall_density.value() * wall_volume +
            fill_density.value() * (geometry.volume - wall_volume);
        break;
    }
    }

    const auto geometry_digest = geometry.canonical_digest();
    if (!geometry_digest) {
        return core::Result<MassDerivationReceipt>::failure(geometry_digest.error());
    }
    const auto construction_digest = construction.canonical_digest();
    if (!construction_digest) {
        return core::Result<MassDerivationReceipt>::failure(construction_digest.error());
    }

    // Distribution model v1: the center of mass follows the mesh volume
    // centroid and the inertia tensor scales the unit-density mesh inertia by
    // (mass / enclosed volume). Recorded in the receipt so any model change is
    // digest-visible.
    const double scale = total_mass / geometry.volume;

    MassDerivationReceipt receipt;
    receipt.geometry_evidence_digest = geometry_digest.value();
    receipt.construction_profile_digest = construction_digest.value();
    receipt.source_revision = construction.source_revision;
    receipt.total_mass_kg = total_mass;
    receipt.total_volume_m3 = geometry.volume;
    receipt.center_of_mass = geometry.volume_centroid;
    receipt.inertia_kg_m2.xx = geometry.unit_density_inertia.xx * scale;
    receipt.inertia_kg_m2.xy = geometry.unit_density_inertia.xy * scale;
    receipt.inertia_kg_m2.xz = geometry.unit_density_inertia.xz * scale;
    receipt.inertia_kg_m2.yy = geometry.unit_density_inertia.yy * scale;
    receipt.inertia_kg_m2.yz = geometry.unit_density_inertia.yz * scale;
    receipt.inertia_kg_m2.zz = geometry.unit_density_inertia.zz * scale;

    if (auto result = receipt.validate(); !result) {
        return core::Result<MassDerivationReceipt>::failure(result.error());
    }
    return core::Result<MassDerivationReceipt>::success(std::move(receipt));
}

} // namespace carto::physical
