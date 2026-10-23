#include <carto/physical/material_intent.hpp>

#include <algorithm>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <tuple>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PHYSICAL_MATERIAL_INTENT";

} // namespace

core::Result<void> PhysicalMaterialIntent::validate() const {
    if (!detail::safe_text(catalog_identity, kMaxIdentityBytes) ||
        catalog_revision.exhausted()) {
        return core::Result<void>::failure(detail::invalid(
            "material intent catalog identity or revision is invalid"));
    }
    if (density_kg_per_m3.has_value() &&
        (!detail::finite_positive(density_kg_per_m3.value()) ||
         density_kg_per_m3.value() > kMaxDensityKgPerM3)) {
        return core::Result<void>::failure(detail::invalid(
            "material intent density qualifier is invalid"));
    }
    if (qualifiers.size() > kMaxQualifierCount ||
        !std::is_sorted(qualifiers.begin(), qualifiers.end()) ||
        std::adjacent_find(qualifiers.begin(), qualifiers.end()) != qualifiers.end()) {
        return core::Result<void>::failure(detail::validation(
            "material intent qualifiers must be bounded, sorted, and unique"));
    }
    for (const auto& [key, value] : qualifiers) {
        if (!detail::safe_text(key, kMaxQualifierKeyBytes) ||
            !detail::safe_text(value, kMaxQualifierValueBytes, true)) {
            return core::Result<void>::failure(detail::invalid(
                "material intent qualifier text is invalid"));
        }
    }
    return core::Result<void>::success();
}

std::string PhysicalMaterialIntent::serialize() const {
    std::ostringstream output;
    detail::encode_intent(output, *this);
    return output.str();
}

core::Result<assets::Sha256Digest> PhysicalMaterialIntent::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<PhysicalMaterialIntent> PhysicalMaterialIntent::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "material intent");
        !result) {
        return core::Result<PhysicalMaterialIntent>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    PhysicalMaterialIntent intent;
    if (auto result = detail::decode_intent(input, intent); !result) {
        return core::Result<PhysicalMaterialIntent>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PhysicalMaterialIntent>::failure(detail::parse_error(
            "material intent contains trailing unbound data"));
    }
    if (auto result = intent.validate(); !result) {
        return core::Result<PhysicalMaterialIntent>::failure(result.error());
    }
    return core::Result<PhysicalMaterialIntent>::success(std::move(intent));
}

namespace detail {

void encode_intent(std::ostream& output, const PhysicalMaterialIntent& intent) {
    output << kMagic << ' ' << PhysicalMaterialIntent::kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "CATALOG " << std::quoted(intent.catalog_identity) << ' ';
    write_revision(output, intent.catalog_revision) << ' ';
    if (intent.density_kg_per_m3.has_value()) {
        output << intent.density_kg_per_m3.value();
    } else {
        output << '-';
    }
    output << '\n';
    output << "QUALIFIERS " << intent.qualifiers.size() << '\n';
    for (const auto& [key, value] : intent.qualifiers) {
        output << "QUALIFIER " << std::quoted(key) << ' ' << std::quoted(value) << '\n';
    }
    output << "END\n";
}

core::Result<void> decode_intent(std::istream& input, PhysicalMaterialIntent& intent) {
    if (auto result = read_frame(input, kMagic, PhysicalMaterialIntent::kSchemaVersion);
        !result) {
        return result;
    }
    std::string density_token;
    if (!require_token(input, "CATALOG") ||
        !(input >> std::quoted(intent.catalog_identity)) ||
        !read_revision(input, intent.catalog_revision) ||
        !(input >> density_token)) {
        return core::Result<void>::failure(parse_error(
            "material intent catalog record is malformed"));
    }
    if (density_token == "-") {
        intent.density_kg_per_m3 = std::nullopt;
    } else {
        const auto parsed = std::from_chars(
            density_token.data(), density_token.data() + density_token.size(),
            intent.density_kg_per_m3.emplace());
        if (parsed.ec != std::errc{} ||
            parsed.ptr != density_token.data() + density_token.size() ||
            !std::isfinite(intent.density_kg_per_m3.value())) {
            return core::Result<void>::failure(parse_error(
                "material intent density qualifier is malformed"));
        }
    }
    std::size_t qualifier_count = 0U;
    if (!require_token(input, "QUALIFIERS") ||
        !read_count(input, PhysicalMaterialIntent::kMaxQualifierCount, qualifier_count)) {
        return core::Result<void>::failure(parse_error(
            "material intent qualifier header is malformed"));
    }
    intent.qualifiers.reserve(qualifier_count);
    for (std::size_t index = 0U; index < qualifier_count; ++index) {
        std::string key;
        std::string value;
        if (!require_token(input, "QUALIFIER") || !(input >> std::quoted(key)) ||
            !(input >> std::quoted(value))) {
            return core::Result<void>::failure(parse_error(
                "material intent qualifier record is malformed"));
        }
        intent.qualifiers.emplace_back(std::move(key), std::move(value));
    }
    return read_end(input, kMagic);
}

} // namespace detail

} // namespace carto::physical
