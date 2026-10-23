#include <carto/physical/surface_ledger.hpp>

#include <carto/physical/construction.hpp>

#include <iomanip>
#include <span>
#include <sstream>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kDamageMagic = "CARTOGRAPHER_PHYSICAL_DAMAGE_SURFACE_DOMAIN";
constexpr std::string_view kShelterMagic = "CARTOGRAPHER_PHYSICAL_SHELTER_SURFACE_DOMAIN";
constexpr std::string_view kLedgerMagic = "CARTOGRAPHER_PHYSICAL_SURFACE_LEDGER";

core::Result<assets::Sha256Digest> digest_text(std::string_view canonical) {
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<void> validate_stratigraphy_binding(
    const surface::MaterialStratigraphy& stratigraphy) {
    if (auto result = stratigraphy.validate(); !result) return result;
    return core::Result<void>::success();
}

} // namespace

namespace detail {

void encode_stratigraphy(
    std::ostream& output, const surface::MaterialStratigraphy& stratigraphy) {
    output << "STRATIGRAPHY " << stratigraphy.domain.value << ' ';
    write_revision(output, stratigraphy.source_revision) << ' '
           << stratigraphy.layers.size() << '\n';
    output << std::setprecision(17);
    for (const auto& layer : stratigraphy.layers) {
        output << "STRATUM " << layer.material << ' ' << layer.nominal_thickness << ' '
               << layer.variation << ' ' << std::quoted(layer.process_class) << '\n';
    }
    output << "STRATIGRAPHY_DIGEST " << stratigraphy.digest << '\n';
}

core::Result<void> decode_stratigraphy(
    std::istream& input, surface::MaterialStratigraphy& stratigraphy) {
    std::size_t layer_count = 0U;
    if (!require_token(input, "STRATIGRAPHY") ||
        !read_integer(input, stratigraphy.domain.value) ||
        !read_revision(input, stratigraphy.source_revision) ||
        !read_count(input, ConstructionProfile::kMaxLayers, layer_count)) {
        return core::Result<void>::failure(parse_error(
            "stratigraphy header is malformed"));
    }
    stratigraphy.layers.clear();
    stratigraphy.layers.reserve(layer_count);
    for (std::size_t index = 0U; index < layer_count; ++index) {
        surface::MaterialLayer layer;
        if (!require_token(input, "STRATUM") ||
            !read_integer(input, layer.material) ||
            !read_double(input, layer.nominal_thickness) ||
            !read_double(input, layer.variation) ||
            !(input >> std::quoted(layer.process_class))) {
            return core::Result<void>::failure(parse_error(
                "stratigraphy layer is malformed"));
        }
        stratigraphy.layers.push_back(std::move(layer));
    }
    if (!require_token(input, "STRATIGRAPHY_DIGEST") ||
        !(input >> stratigraphy.digest) || stratigraphy.digest.size() != 64U) {
        return core::Result<void>::failure(parse_error(
            "stratigraphy digest is malformed"));
    }
    return core::Result<void>::success();
}

} // namespace detail

core::Result<void> DamageSurfaceDomain::validate() const {
    if (!domain || source_revision.exhausted() || damage_region == 0U ||
        !detail::safe_text(damage_class, kMaxClassBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "damage surface domain identity, revision, region, or class is invalid"));
    }
    return validate_stratigraphy_binding(stratigraphy);
}

core::Result<surface::MaterialExposure> DamageSurfaceDomain::exposure(
    double damage_depth) const {
    return surface::resolve_material_exposure(stratigraphy, damage_depth);
}

std::string DamageSurfaceDomain::serialize() const {
    std::ostringstream output;
    output << kDamageMagic << ' ' << kSchemaVersion << '\n';
    detail::encode_stratigraphy(output, stratigraphy);
    output << "DAMAGE " << damage_region << ' ' << std::quoted(damage_class) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> DamageSurfaceDomain::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return digest_text(serialize());
}

core::Result<DamageSurfaceDomain> DamageSurfaceDomain::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "damage surface domain");
        !result) {
        return core::Result<DamageSurfaceDomain>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    DamageSurfaceDomain record;
    if (auto result = detail::read_frame(input, kDamageMagic, kSchemaVersion);
        !result) {
        return core::Result<DamageSurfaceDomain>::failure(result.error());
    }
    if (auto result = detail::decode_stratigraphy(input, record.stratigraphy); !result) {
        return core::Result<DamageSurfaceDomain>::failure(result.error());
    }
    // The bound stratigraphy is the single source of identity on the wire.
    record.domain = record.stratigraphy.domain;
    record.source_revision = record.stratigraphy.source_revision;
    if (!detail::require_token(input, "DAMAGE") ||
        !detail::read_integer(input, record.damage_region) ||
        !(input >> std::quoted(record.damage_class))) {
        return core::Result<DamageSurfaceDomain>::failure(detail::parse_error(
            "damage surface domain record is malformed"));
    }
    if (auto result = detail::read_end(input, kDamageMagic); !result) {
        return core::Result<DamageSurfaceDomain>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<DamageSurfaceDomain>::failure(detail::parse_error(
            "damage surface domain contains trailing unbound data"));
    }
    if (auto result = record.validate(); !result) {
        return core::Result<DamageSurfaceDomain>::failure(result.error());
    }
    return core::Result<DamageSurfaceDomain>::success(std::move(record));
}

core::Result<void> ShelterSurfaceDomain::validate() const {
    if (!domain || source_revision.exhausted() ||
        !detail::safe_text(shelter_class, kMaxClassBytes) ||
        !detail::finite_unit(coverage)) {
        return core::Result<void>::failure(detail::invalid(
            "shelter surface domain identity, revision, class, or coverage is invalid"));
    }
    return validate_stratigraphy_binding(stratigraphy);
}

core::Result<surface::MaterialExposure> ShelterSurfaceDomain::sheltered_material()
    const {
    return surface::resolve_material_exposure(stratigraphy, 0.0);
}

std::string ShelterSurfaceDomain::serialize() const {
    std::ostringstream output;
    output << kShelterMagic << ' ' << kSchemaVersion << '\n';
    detail::encode_stratigraphy(output, stratigraphy);
    output << std::setprecision(17) << "SHELTER " << std::quoted(shelter_class) << ' '
           << coverage << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ShelterSurfaceDomain::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return digest_text(serialize());
}

core::Result<ShelterSurfaceDomain> ShelterSurfaceDomain::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "shelter surface domain");
        !result) {
        return core::Result<ShelterSurfaceDomain>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    ShelterSurfaceDomain record;
    if (auto result = detail::read_frame(input, kShelterMagic, kSchemaVersion);
        !result) {
        return core::Result<ShelterSurfaceDomain>::failure(result.error());
    }
    if (auto result = detail::decode_stratigraphy(input, record.stratigraphy); !result) {
        return core::Result<ShelterSurfaceDomain>::failure(result.error());
    }
    // The bound stratigraphy is the single source of identity on the wire.
    record.domain = record.stratigraphy.domain;
    record.source_revision = record.stratigraphy.source_revision;
    if (!detail::require_token(input, "SHELTER") ||
        !(input >> std::quoted(record.shelter_class)) ||
        !detail::read_double(input, record.coverage)) {
        return core::Result<ShelterSurfaceDomain>::failure(detail::parse_error(
            "shelter surface domain record is malformed"));
    }
    if (auto result = detail::read_end(input, kShelterMagic); !result) {
        return core::Result<ShelterSurfaceDomain>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ShelterSurfaceDomain>::failure(detail::parse_error(
            "shelter surface domain contains trailing unbound data"));
    }
    if (auto result = record.validate(); !result) {
        return core::Result<ShelterSurfaceDomain>::failure(result.error());
    }
    return core::Result<ShelterSurfaceDomain>::success(std::move(record));
}

core::Result<void> PhysicalSurfaceEntry::validate() const {
    if (!domain || source_revision.exhausted()) {
        return core::Result<void>::failure(detail::invalid(
            "surface ledger entry identity or revision is invalid"));
    }
    if (!render_material_region.has_value() && !physics_surface_identity.has_value() &&
        !damage_region.has_value() && !audio_surface_identity.has_value() &&
        !weather_exposure.has_value()) {
        return core::Result<void>::failure(detail::validation(
            "surface ledger entry carries no consumer binding"));
    }
    if (render_material_region.has_value() && render_material_region.value() == 0U) {
        return core::Result<void>::failure(detail::invalid(
            "surface ledger render material region is invalid"));
    }
    if (damage_region.has_value() && damage_region.value() == 0U) {
        return core::Result<void>::failure(detail::invalid(
            "surface ledger damage region is invalid"));
    }
    if (physics_surface_identity.has_value() &&
        !detail::safe_text(physics_surface_identity.value(), PhysicalSurfaceLedger::kMaxIdentityBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "surface ledger physics surface identity is invalid"));
    }
    if (audio_surface_identity.has_value() &&
        !detail::safe_text(audio_surface_identity.value(), PhysicalSurfaceLedger::kMaxIdentityBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "surface ledger audio surface identity is invalid"));
    }
    if (weather_exposure.has_value()) {
        if (auto result = weather_exposure->validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> PhysicalSurfaceLedger::insert(PhysicalSurfaceEntry entry) {
    if (auto result = entry.validate(); !result) return result;
    if (entries_.size() >= kMaxEntries) {
        return core::Result<void>::failure(detail::validation(
            "surface ledger has reached its bounded capacity"));
    }
    const auto [iterator, inserted] =
        entries_.emplace(entry.domain, std::move(entry));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(detail::invalid(
            "surface ledger domain identity is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<void> PhysicalSurfaceLedger::validate() const {
    if (entries_.size() > kMaxEntries) {
        return core::Result<void>::failure(detail::validation(
            "surface ledger exceeds its bounded capacity"));
    }
    for (const auto& [identity, entry] : entries_) {
        if (identity != entry.domain) {
            return core::Result<void>::failure(detail::validation(
                "surface ledger map identity does not match its entry"));
        }
        if (auto result = entry.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string PhysicalSurfaceLedger::serialize() const {
    std::ostringstream output;
    detail::encode_ledger(output, *this);
    return output.str();
}

core::Result<assets::Sha256Digest> PhysicalSurfaceLedger::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return digest_text(serialize());
}

core::Result<PhysicalSurfaceLedger> PhysicalSurfaceLedger::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "surface ledger");
        !result) {
        return core::Result<PhysicalSurfaceLedger>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    PhysicalSurfaceLedger ledger;
    if (auto result = detail::decode_ledger(input, ledger); !result) {
        return core::Result<PhysicalSurfaceLedger>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PhysicalSurfaceLedger>::failure(detail::parse_error(
            "surface ledger contains trailing unbound data"));
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<PhysicalSurfaceLedger>::failure(result.error());
    }
    return core::Result<PhysicalSurfaceLedger>::success(std::move(ledger));
}

namespace detail {

void encode_ledger(std::ostream& output, const PhysicalSurfaceLedger& ledger) {
    output << kLedgerMagic << ' ' << PhysicalSurfaceLedger::kSchemaVersion << '\n';
    output << "ENTRIES " << ledger.entries().size() << '\n';
    for (const auto& [identity, entry] : ledger.entries()) {
        static_cast<void>(identity);
        output << "ENTRY ";
        write_revision(output, entry.source_revision) << '\n';
        output << "DOMAIN " << entry.domain.value << '\n';
        output << "RENDER_REGION " << (entry.render_material_region.has_value() ? 1 : 0)
               << ' ';
        if (entry.render_material_region.has_value()) {
            output << entry.render_material_region.value();
        } else {
            output << '-';
        }
        output << '\n';
        output << "PHYSICS_SURFACE " << (entry.physics_surface_identity.has_value() ? 1 : 0)
               << ' ';
        if (entry.physics_surface_identity.has_value()) {
            output << std::quoted(entry.physics_surface_identity.value());
        } else {
            output << '-';
        }
        output << '\n';
        output << "DAMAGE_REGION " << (entry.damage_region.has_value() ? 1 : 0) << ' ';
        if (entry.damage_region.has_value()) {
            output << entry.damage_region.value();
        } else {
            output << '-';
        }
        output << '\n';
        output << "AUDIO_SURFACE " << (entry.audio_surface_identity.has_value() ? 1 : 0)
               << ' ';
        if (entry.audio_surface_identity.has_value()) {
            output << std::quoted(entry.audio_surface_identity.value());
        } else {
            output << '-';
        }
        output << '\n';
        output << "WEATHER_EXPOSURE "
               << (entry.weather_exposure.has_value() ? 1 : 0) << '\n';
        if (entry.weather_exposure.has_value()) {
            encode_stratigraphy(output, entry.weather_exposure.value());
        }
    }
    output << "END\n";
}

core::Result<void> decode_ledger(std::istream& input, PhysicalSurfaceLedger& ledger) {
    if (auto result = read_frame(input, kLedgerMagic, PhysicalSurfaceLedger::kSchemaVersion);
        !result) {
        return result;
    }
    std::size_t entry_count = 0U;
    if (!require_token(input, "ENTRIES") ||
        !read_count(input, PhysicalSurfaceLedger::kMaxEntries, entry_count)) {
        return core::Result<void>::failure(parse_error(
            "surface ledger entry header is malformed"));
    }
    // Absent optional fields are written as a "-" placeholder; consume it.
    const auto skip_placeholder = [&input]() {
        std::string placeholder;
        return static_cast<bool>(input >> placeholder) && placeholder == "-";
    };
    for (std::size_t index = 0U; index < entry_count; ++index) {
        PhysicalSurfaceEntry entry;
        std::uint64_t domain = 0U;
        if (!require_token(input, "ENTRY") ||
            !read_revision(input, entry.source_revision) ||
            !require_token(input, "DOMAIN") || !read_integer(input, domain)) {
            return core::Result<void>::failure(parse_error(
                "surface ledger entry identity is malformed"));
        }
        entry.domain = surface::SurfaceDomainId{domain};
        unsigned present = 0U;
        std::uint64_t region = 0U;
        if (!require_token(input, "RENDER_REGION") || !read_integer(input, present) ||
            present > 1U) {
            return core::Result<void>::failure(parse_error(
                "surface ledger render region record is malformed"));
        }
        if (present != 0U) {
            if (!read_integer(input, region) || region == 0U ||
                region > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<void>::failure(parse_error(
                    "surface ledger render region is malformed"));
            }
            entry.render_material_region = static_cast<std::uint32_t>(region);
        } else if (!skip_placeholder()) {
            return core::Result<void>::failure(parse_error(
                "surface ledger render region placeholder is malformed"));
        }
        if (!require_token(input, "PHYSICS_SURFACE") ||
            !read_integer(input, present) || present > 1U) {
            return core::Result<void>::failure(parse_error(
                "surface ledger physics surface record is malformed"));
        }
        if (present != 0U) {
            std::string identity;
            if (!(input >> std::quoted(identity))) {
                return core::Result<void>::failure(parse_error(
                    "surface ledger physics surface identity is malformed"));
            }
            entry.physics_surface_identity = std::move(identity);
        } else if (!skip_placeholder()) {
            return core::Result<void>::failure(parse_error(
                "surface ledger physics surface placeholder is malformed"));
        }
        if (!require_token(input, "DAMAGE_REGION") || !read_integer(input, present) ||
            present > 1U) {
            return core::Result<void>::failure(parse_error(
                "surface ledger damage region record is malformed"));
        }
        if (present != 0U) {
            if (!read_integer(input, region) || region == 0U ||
                region > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<void>::failure(parse_error(
                    "surface ledger damage region is malformed"));
            }
            entry.damage_region = static_cast<std::uint32_t>(region);
        } else if (!skip_placeholder()) {
            return core::Result<void>::failure(parse_error(
                "surface ledger damage region placeholder is malformed"));
        }
        if (!require_token(input, "AUDIO_SURFACE") || !read_integer(input, present) ||
            present > 1U) {
            return core::Result<void>::failure(parse_error(
                "surface ledger audio surface record is malformed"));
        }
        if (present != 0U) {
            std::string identity;
            if (!(input >> std::quoted(identity))) {
                return core::Result<void>::failure(parse_error(
                    "surface ledger audio surface identity is malformed"));
            }
            entry.audio_surface_identity = std::move(identity);
        } else if (!skip_placeholder()) {
            return core::Result<void>::failure(parse_error(
                "surface ledger audio surface placeholder is malformed"));
        }
        if (!require_token(input, "WEATHER_EXPOSURE") ||
            !read_integer(input, present) || present > 1U) {
            return core::Result<void>::failure(parse_error(
                "surface ledger weather exposure record is malformed"));
        }
        if (present != 0U) {
            entry.weather_exposure.emplace();
            if (auto result = decode_stratigraphy(input, entry.weather_exposure.value());
                !result) {
                return result;
            }
        }
        if (auto result = ledger.insert(std::move(entry)); !result) {
            return core::Result<void>::failure(result.error());
        }
    }
    return read_end(input, kLedgerMagic);
}

} // namespace detail

} // namespace carto::physical
