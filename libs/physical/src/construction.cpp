#include <carto/physical/construction.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <span>
#include <sstream>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PHYSICAL_CONSTRUCTION_PROFILE";
constexpr double kThicknessCorrespondenceEpsilon = 1e-9;

bool valid_kind(ConstructionKind kind) {
    switch (kind) {
    case ConstructionKind::solid:
    case ConstructionKind::shell:
    case ConstructionKind::layered:
    case ConstructionKind::filled:
        return true;
    }
    return false;
}

bool valid_thickness_source(ThicknessSource source) {
    switch (source) {
    case ThicknessSource::authored:
    case ThicknessSource::derived_from_geometry:
        return true;
    }
    return false;
}

} // namespace

std::string_view construction_kind_name(ConstructionKind kind) noexcept {
    switch (kind) {
    case ConstructionKind::solid: return "solid";
    case ConstructionKind::shell: return "shell";
    case ConstructionKind::layered: return "layered";
    case ConstructionKind::filled: return "filled";
    }
    return "unknown";
}

std::string_view thickness_source_name(ThicknessSource source) noexcept {
    switch (source) {
    case ThicknessSource::authored: return "authored";
    case ThicknessSource::derived_from_geometry: return "derived-from-geometry";
    }
    return "unknown";
}

core::Result<void> ConstructionLayer::validate() const {
    if (!detail::finite_positive(nominal_thickness) ||
        nominal_thickness > ConstructionProfile::kMaxWallThicknessMeters) {
        return core::Result<void>::failure(detail::invalid(
            "construction layer thickness is invalid"));
    }
    if (auto result = material.validate(); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> ConstructionProfile::validate() const {
    if (!valid_kind(kind) || source_revision.exhausted()) {
        return core::Result<void>::failure(detail::invalid(
            "construction profile kind or source revision is invalid"));
    }
    if (auto result = primary_material.validate(); !result) return result;

    const bool has_wall = wall_thickness.has_value() || wall_thickness_source.has_value();
    switch (kind) {
    case ConstructionKind::solid:
        if (has_wall || !layers.empty() || fill_material.has_value() ||
            stratigraphy.has_value()) {
            return core::Result<void>::failure(detail::validation(
                "solid construction profile carries non-solid fields"));
        }
        break;
    case ConstructionKind::shell:
        if (!wall_thickness.has_value() || !wall_thickness_source.has_value() ||
            !detail::finite_positive(wall_thickness.value()) ||
            wall_thickness.value() > kMaxWallThicknessMeters ||
            !valid_thickness_source(wall_thickness_source.value()) ||
            !layers.empty() || fill_material.has_value() || stratigraphy.has_value()) {
            return core::Result<void>::failure(detail::validation(
                "shell construction profile requires exactly one wall declaration"));
        }
        break;
    case ConstructionKind::layered:
        if (has_wall || fill_material.has_value() || layers.empty() ||
            layers.size() > kMaxLayers || !stratigraphy.has_value()) {
            return core::Result<void>::failure(detail::validation(
                "layered construction profile requires layers and stratigraphy"));
        }
        for (const auto& layer : layers) {
            if (auto result = layer.validate(); !result) return result;
        }
        if (auto result = stratigraphy->validate(); !result) return result;
        if (stratigraphy->layers.size() != layers.size()) {
            return core::Result<void>::failure(detail::validation(
                "construction layers do not correspond to the bound stratigraphy"));
        }
        for (std::size_t index = 0U; index < layers.size(); ++index) {
            if (std::abs(layers[index].nominal_thickness -
                         stratigraphy->layers[index].nominal_thickness) >
                kThicknessCorrespondenceEpsilon) {
                return core::Result<void>::failure(detail::validation(
                    "construction layer thickness drifted from the bound stratigraphy"));
            }
        }
        break;
    case ConstructionKind::filled:
        if (!wall_thickness.has_value() || !wall_thickness_source.has_value() ||
            !detail::finite_positive(wall_thickness.value()) ||
            wall_thickness.value() > kMaxWallThicknessMeters ||
            !valid_thickness_source(wall_thickness_source.value()) ||
            !fill_material.has_value() || !layers.empty() || stratigraphy.has_value()) {
            return core::Result<void>::failure(detail::validation(
                "filled construction profile requires a wall and a fill material"));
        }
        if (auto result = fill_material->validate(); !result) return result;
        break;
    }
    return core::Result<void>::success();
}

std::string ConstructionProfile::serialize() const {
    std::ostringstream output;
    detail::encode_construction(output, *this);
    return output.str();
}

core::Result<assets::Sha256Digest> ConstructionProfile::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<ConstructionProfile> ConstructionProfile::deserialize(std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "construction profile");
        !result) {
        return core::Result<ConstructionProfile>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    ConstructionProfile profile;
    if (auto result = detail::decode_construction(input, profile); !result) {
        return core::Result<ConstructionProfile>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ConstructionProfile>::failure(detail::parse_error(
            "construction profile contains trailing unbound data"));
    }
    if (auto result = profile.validate(); !result) {
        return core::Result<ConstructionProfile>::failure(result.error());
    }
    return core::Result<ConstructionProfile>::success(std::move(profile));
}

namespace detail {

void encode_construction(std::ostream& output, const ConstructionProfile& profile) {
    output << kMagic << ' ' << ConstructionProfile::kSchemaVersion << '\n';
    output << "KIND " << static_cast<unsigned>(profile.kind) << ' ';
    write_revision(output, profile.source_revision) << '\n';
    output << "PRIMARY_MATERIAL\n";
    encode_intent(output, profile.primary_material);
    output << "WALL ";
    if (profile.wall_thickness.has_value()) {
        output << std::setprecision(17) << profile.wall_thickness.value() << ' ';
    } else {
        output << "- ";
    }
    if (profile.wall_thickness_source.has_value()) {
        output << static_cast<unsigned>(profile.wall_thickness_source.value());
    } else {
        output << '-';
    }
    output << '\n';
    output << "LAYERS " << profile.layers.size() << '\n';
    for (const auto& layer : profile.layers) {
        output << "LAYER " << std::setprecision(17) << layer.nominal_thickness << '\n';
        encode_intent(output, layer.material);
    }
    output << "FILL_MATERIAL " << (profile.fill_material.has_value() ? 1 : 0) << '\n';
    if (profile.fill_material.has_value()) {
        encode_intent(output, profile.fill_material.value());
    }
    output << "BOUND_STRATIGRAPHY " << (profile.stratigraphy.has_value() ? 1 : 0)
           << '\n';
    if (profile.stratigraphy.has_value()) {
        encode_stratigraphy(output, profile.stratigraphy.value());
    }
    output << "END\n";
}

core::Result<void> decode_construction(std::istream& input, ConstructionProfile& profile) {
    if (auto result = read_frame(input, kMagic, ConstructionProfile::kSchemaVersion);
        !result) {
        return result;
    }
    unsigned kind = 0U;
    if (!require_token(input, "KIND") || !read_integer(input, kind) ||
        !read_revision(input, profile.source_revision)) {
        return core::Result<void>::failure(parse_error(
            "construction profile header is malformed"));
    }
    profile.kind = static_cast<ConstructionKind>(kind);
    if (!require_token(input, "PRIMARY_MATERIAL")) {
        return core::Result<void>::failure(parse_error(
            "construction profile primary material is malformed"));
    }
    if (auto result = decode_intent(input, profile.primary_material); !result) return result;
    if (!require_token(input, "WALL")) {
        return core::Result<void>::failure(parse_error(
            "construction profile wall record is malformed"));
    }
    std::string thickness_token;
    std::string source_token;
    if (!(input >> thickness_token) || !(input >> source_token)) {
        return core::Result<void>::failure(parse_error(
            "construction profile wall record is malformed"));
    }
    if (thickness_token == "-") {
        profile.wall_thickness = std::nullopt;
    } else {
        double thickness = 0.0;
        const auto parsed = std::from_chars(
            thickness_token.data(), thickness_token.data() + thickness_token.size(),
            thickness);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != thickness_token.data() + thickness_token.size() ||
            !std::isfinite(thickness)) {
            return core::Result<void>::failure(parse_error(
                "construction profile wall thickness is malformed"));
        }
        profile.wall_thickness = thickness;
    }
    if (source_token == "-") {
        profile.wall_thickness_source = std::nullopt;
    } else {
        const auto parsed = std::from_chars(
            source_token.data(), source_token.data() + source_token.size(), kind);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != source_token.data() + source_token.size()) {
            return core::Result<void>::failure(parse_error(
                "construction profile wall thickness source is malformed"));
        }
        profile.wall_thickness_source = static_cast<ThicknessSource>(kind);
    }
    std::size_t layer_count = 0U;
    if (!require_token(input, "LAYERS") ||
        !read_count(input, ConstructionProfile::kMaxLayers, layer_count)) {
        return core::Result<void>::failure(parse_error(
            "construction profile layer header is malformed"));
    }
    profile.layers.clear();
    profile.layers.reserve(layer_count);
    for (std::size_t index = 0U; index < layer_count; ++index) {
        ConstructionLayer layer;
        if (!require_token(input, "LAYER") ||
            !read_double(input, layer.nominal_thickness)) {
            return core::Result<void>::failure(parse_error(
                "construction profile layer record is malformed"));
        }
        if (auto result = decode_intent(input, layer.material); !result) return result;
        profile.layers.push_back(std::move(layer));
    }
    unsigned has_fill = 0U;
    if (!require_token(input, "FILL_MATERIAL") || !read_integer(input, has_fill) ||
        has_fill > 1U) {
        return core::Result<void>::failure(parse_error(
            "construction profile fill record is malformed"));
    }
    if (has_fill != 0U) {
        profile.fill_material.emplace();
        if (auto result = decode_intent(input, profile.fill_material.value()); !result) {
            return result;
        }
    } else {
        profile.fill_material = std::nullopt;
    }
    unsigned has_stratigraphy = 0U;
    if (!require_token(input, "BOUND_STRATIGRAPHY") ||
        !read_integer(input, has_stratigraphy) || has_stratigraphy > 1U) {
        return core::Result<void>::failure(parse_error(
            "construction profile stratigraphy record is malformed"));
    }
    if (has_stratigraphy != 0U) {
        profile.stratigraphy.emplace();
        if (auto result = decode_stratigraphy(input, profile.stratigraphy.value());
            !result) {
            return result;
        }
    } else {
        profile.stratigraphy = std::nullopt;
    }
    return read_end(input, kMagic);
}

} // namespace detail

} // namespace carto::physical
