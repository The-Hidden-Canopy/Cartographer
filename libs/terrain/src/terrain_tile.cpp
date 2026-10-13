#include <carto/terrain/terrain_tile.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <system_error>
#include <utility>

namespace carto::terrain {
namespace {

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic stale(std::string message) {
    return {core::ErrorCode::stale_data, std::move(message)};
}

core::Diagnostic validation(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

std::size_t cell_index(std::uint32_t u, std::uint32_t v, std::uint32_t width) {
    return static_cast<std::size_t>(v) * width + u;
}

core::Result<assets::Sha256Digest> digest_of(
    const spatial_field::ScalarField2D& source) {
    return source.canonical_digest();
}

core::Result<void> validate_source(
    const spatial_field::ScalarField2D& source,
    TileKey key) {
    if (auto result = source.validate(); !result) return result;
    if (source.semantic() != spatial_field::FieldSemantic::terrain_elevation) {
        return core::Result<void>::failure(validation(
            "terrain derivation requires a terrain_elevation source field"));
    }
    if (source.identity() != terrain_tile_identity(key)) {
        return core::Result<void>::failure(validation(
            "terrain source identity does not match the requested tile key"));
    }
    return core::Result<void>::success();
}

core::Vec3d normal_at(
    const spatial_field::ScalarField2D& source,
    std::uint32_t u,
    std::uint32_t v) {
    const auto width = source.width();
    const auto height = source.height();
    const auto& values = source.values();
    const std::uint32_t left = u == 0U ? u : u - 1U;
    const std::uint32_t right = std::min(u + 1U, width - 1U);
    const std::uint32_t down = v == 0U ? v : v - 1U;
    const std::uint32_t up = std::min(v + 1U, height - 1U);
    const double dx = static_cast<double>(right - left) * source.cell_size();
    const double dz = static_cast<double>(up - down) * source.cell_size();
    const double dhdx = (values[cell_index(right, v, width)] -
        values[cell_index(left, v, width)]) / dx;
    const double dhdz = (values[cell_index(u, up, width)] -
        values[cell_index(u, down, width)]) / dz;
    return core::Vec3d{-dhdx, 1.0, -dhdz}.normalized();
}

} // namespace

namespace detail {

struct TerrainTileBuilder final {
    static void update_cell(
        TerrainTileProduct& product,
        const spatial_field::ScalarField2D& source,
        std::uint32_t u,
        std::uint32_t v) {
        const auto width = source.width();
        const auto& values = source.values();
        const double h00 = values[cell_index(u, v, width)];
        const double h10 = values[cell_index(u + 1U, v, width)];
        const double h01 = values[cell_index(u, v + 1U, width)];
        const double h11 = values[cell_index(u + 1U, v + 1U, width)];
        const double inverse_span = 1.0 / (2.0 * source.cell_size());
        const double dhdu = ((h10 - h00) + (h11 - h01)) * inverse_span;
        const double dhdv = ((h01 - h00) + (h11 - h10)) * inverse_span;
        const double slope = std::atan(std::hypot(dhdu, dhdv));
        const auto index = cell_index(u, v, width - 1U);
        product.cell_slope_radians_[index] = slope;
        product.navigation_passable_[index] = static_cast<std::uint8_t>(
            slope <= product.max_navigation_slope_radians_);
        if (product.water_level_.has_value()) {
            const double center_height = (h00 + h10 + h01 + h11) * 0.25;
            product.wet_[index] = static_cast<std::uint8_t>(
                *product.water_level_ > center_height);
        }
    }

    static core::Result<void> update_metadata(
        TerrainTileProduct& product,
        const spatial_field::ScalarField2D& source) {
        const auto digest = digest_of(source);
        if (!digest) return core::Result<void>::failure(digest.error());
        product.source_revision_ = source.source_revision();
        product.source_digest_ = digest.value();
        product.source_origin_uv_ = source.origin_uv();
        product.source_cell_size_ = source.cell_size();
        product.source_width_ = source.width();
        product.source_height_ = source.height();
        return core::Result<void>::success();
    }
};

} // namespace detail

std::string terrain_tile_identity(TileKey key) {
    return "terrain.tile.x" + std::to_string(key.x) + ".z" + std::to_string(key.z);
}

core::Result<TileKey> parse_terrain_tile_identity(std::string_view identity) {
    constexpr std::string_view prefix = "terrain.tile.x";
    if (!identity.starts_with(prefix)) {
        return core::Result<TileKey>::failure(invalid("terrain tile identity prefix is invalid"));
    }
    const auto separator = identity.find(".z", prefix.size());
    if (separator == std::string_view::npos || separator == prefix.size() ||
        separator + 2U >= identity.size()) {
        return core::Result<TileKey>::failure(invalid("terrain tile identity coordinates are malformed"));
    }
    TileKey key;
    const auto parse_coordinate = [](std::string_view text, std::int32_t& output) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    if (!parse_coordinate(identity.substr(prefix.size(), separator - prefix.size()), key.x) ||
        !parse_coordinate(identity.substr(separator + 2U), key.z) ||
        terrain_tile_identity(key) != identity) {
        return core::Result<TileKey>::failure(invalid(
            "terrain tile identity coordinates are invalid or non-canonical"));
    }
    return core::Result<TileKey>::success(key);
}

core::Result<hydro::BathymetryField> to_bathymetry_field(
    const spatial_field::ScalarField2D& source,
    std::string coordinate_frame) {
    if (auto result = source.validate(); !result) {
        return core::Result<hydro::BathymetryField>::failure(result.error());
    }
    if (source.semantic() != spatial_field::FieldSemantic::terrain_elevation ||
        !parse_terrain_tile_identity(source.identity())) {
        return core::Result<hydro::BathymetryField>::failure(validation(
            "hydro bathymetry requires a canonical terrain elevation tile source"));
    }
    const auto digest = digest_of(source);
    if (!digest) return core::Result<hydro::BathymetryField>::failure(digest.error());
    hydro::BathymetryField bathymetry;
    bathymetry.source.domain_id = source.identity();
    bathymetry.source.source_revision = source.source_revision();
    bathymetry.source.source_digest = digest.value();
    bathymetry.source.coordinate_frame = std::move(coordinate_frame);
    bathymetry.source.provenance = "Cartographer terrain elevation source adapter";
    bathymetry.terrain_identity = source.identity();
    bathymetry.terrain_revision = source.source_revision();
    bathymetry.terrain_digest = digest.value();
    bathymetry.origin_xy = source.origin_uv();
    bathymetry.cell_size_m = source.cell_size();
    bathymetry.width = source.width() - 1U;
    bathymetry.height = source.height() - 1U;
    bathymetry.bed_elevations_m.reserve(
        static_cast<std::size_t>(bathymetry.width) * bathymetry.height);
    for (std::uint32_t v = 0U; v < bathymetry.height; ++v) {
        for (std::uint32_t u = 0U; u < bathymetry.width; ++u) {
            const auto width = source.width();
            const double h00 = source.values()[cell_index(u, v, width)];
            const double h10 = source.values()[cell_index(u + 1U, v, width)];
            const double h01 = source.values()[cell_index(u, v + 1U, width)];
            const double h11 = source.values()[cell_index(u + 1U, v + 1U, width)];
            bathymetry.bed_elevations_m.push_back((h00 + h10 + h01 + h11) * 0.25);
        }
    }
    if (auto result = bathymetry.validate(); !result) {
        return core::Result<hydro::BathymetryField>::failure(result.error());
    }
    return core::Result<hydro::BathymetryField>::success(std::move(bathymetry));
}

core::Result<void> TerrainTileProduct::validate() const {
    const auto parsed_key = parse_terrain_tile_identity(source_identity_);
    const std::uint64_t vertex_count = static_cast<std::uint64_t>(mesh_u_.size()) * mesh_v_.size();
    const std::uint64_t cell_count = static_cast<std::uint64_t>(source_width_ -
        (source_width_ > 0U ? 1U : 0U)) * (source_height_ - (source_height_ > 0U ? 1U : 0U));
    if (!parsed_key || parsed_key.value() != tile_key_ || source_revision_.exhausted() ||
        source_digest_.is_zero() || source_width_ < 2U || source_height_ < 2U ||
        source_width_ > spatial_field::ScalarField2D::kMaxAxisSamples ||
        source_height_ > spatial_field::ScalarField2D::kMaxAxisSamples ||
        !source_origin_uv_.finite() || !std::isfinite(source_cell_size_) || source_cell_size_ <= 0.0 ||
        lod_step_ == 0U || !std::isfinite(max_navigation_slope_radians_) ||
        max_navigation_slope_radians_ < 0.0 || max_navigation_slope_radians_ > std::numbers::pi / 2.0 ||
        (water_level_.has_value() && !std::isfinite(*water_level_)) ||
        vertex_count != vertices_.size() || mesh_u_.size() < 2U || mesh_v_.size() < 2U ||
        indices_.size() != (mesh_u_.size() - 1U) * (mesh_v_.size() - 1U) * 6U ||
        cell_slope_radians_.size() != cell_count || navigation_passable_.size() != cell_count ||
        (water_level_.has_value() ? wet_.size() != cell_count : !wet_.empty())) {
        return core::Result<void>::failure(validation("terrain tile product header or arrays are invalid"));
    }
    for (std::size_t index = 0U; index < vertices_.size(); ++index) {
        if (!vertices_[index].position.finite() || !vertices_[index].normal.finite() ||
            std::abs(vertices_[index].normal.length() - 1.0) > 1.0e-8) {
            return core::Result<void>::failure(validation("terrain mesh has an invalid vertex or normal"));
        }
    }
    if (std::any_of(indices_.begin(), indices_.end(), [this](std::uint32_t index) {
            return index >= vertices_.size(); }) ||
        std::any_of(cell_slope_radians_.begin(), cell_slope_radians_.end(), [](double slope) {
            return !std::isfinite(slope) || slope < 0.0 || slope > std::numbers::pi / 2.0; }) ||
        std::any_of(navigation_passable_.begin(), navigation_passable_.end(), [](std::uint8_t bit) {
            return bit > 1U; }) ||
        std::any_of(wet_.begin(), wet_.end(), [](std::uint8_t bit) { return bit > 1U; })) {
        return core::Result<void>::failure(validation("terrain tile product contains invalid samples"));
    }
    const auto valid_axis = [](const std::vector<std::uint32_t>& axis, std::uint32_t last) {
        if (axis.front() != 0U || axis.back() != last) return false;
        for (std::size_t index = 1U; index < axis.size(); ++index) {
            if (axis[index] <= axis[index - 1U] || axis[index] > last) return false;
        }
        return true;
    };
    if (!valid_axis(mesh_u_, source_width_ - 1U) || !valid_axis(mesh_v_, source_height_ - 1U)) {
        return core::Result<void>::failure(validation("terrain LOD sample axes are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<TerrainTileProduct> build_terrain_tile(
    const spatial_field::ScalarField2D& source,
    TileKey key,
    std::uint32_t lod_step,
    double max_navigation_slope_degrees,
    std::optional<double> water_level) {
    if (auto result = validate_source(source, key); !result) {
        return core::Result<TerrainTileProduct>::failure(result.error());
    }
    if (lod_step == 0U || lod_step >= source.width() || lod_step >= source.height() ||
        !std::isfinite(max_navigation_slope_degrees) || max_navigation_slope_degrees < 0.0 ||
        max_navigation_slope_degrees > 90.0 ||
        (water_level.has_value() && !std::isfinite(*water_level))) {
        return core::Result<TerrainTileProduct>::failure(invalid(
            "terrain LOD, navigation slope, or water level is invalid"));
    }
    const auto digest = digest_of(source);
    if (!digest) return core::Result<TerrainTileProduct>::failure(digest.error());

    TerrainTileProduct product;
    product.source_identity_ = source.identity();
    product.source_revision_ = source.source_revision();
    product.source_digest_ = digest.value();
    product.tile_key_ = key;
    product.source_origin_uv_ = source.origin_uv();
    product.source_cell_size_ = source.cell_size();
    product.source_width_ = source.width();
    product.source_height_ = source.height();
    product.lod_step_ = lod_step;
    product.max_navigation_slope_radians_ = max_navigation_slope_degrees *
        std::numbers::pi / 180.0;
    product.water_level_ = water_level;
    const auto make_axis = [lod_step](std::uint32_t sample_count) {
        std::vector<std::uint32_t> axis;
        for (std::uint32_t value = 0U; value < sample_count - 1U; value += lod_step) {
            axis.push_back(value);
        }
        if (axis.empty() || axis.back() != sample_count - 1U) axis.push_back(sample_count - 1U);
        return axis;
    };
    product.mesh_u_ = make_axis(source.width());
    product.mesh_v_ = make_axis(source.height());
    product.vertices_.reserve(product.mesh_u_.size() * product.mesh_v_.size());
    const auto& values = source.values();
    for (const auto v : product.mesh_v_) {
        for (const auto u : product.mesh_u_) {
            const auto sample = cell_index(u, v, source.width());
            product.vertices_.push_back({
                {source.origin_uv().x + u * source.cell_size(), values[sample],
                 source.origin_uv().y + v * source.cell_size()},
                normal_at(source, u, v)});
        }
    }
    product.indices_.reserve((product.mesh_u_.size() - 1U) * (product.mesh_v_.size() - 1U) * 6U);
    for (std::uint32_t v = 0U; v + 1U < product.mesh_v_.size(); ++v) {
        for (std::uint32_t u = 0U; u + 1U < product.mesh_u_.size(); ++u) {
            const auto a = v * static_cast<std::uint32_t>(product.mesh_u_.size()) + u;
            const auto b = a + 1U;
            const auto c = a + static_cast<std::uint32_t>(product.mesh_u_.size());
            const auto d = c + 1U;
            product.indices_.insert(product.indices_.end(), {a, c, b, b, c, d});
        }
    }
    const auto cell_count = static_cast<std::size_t>(source.width() - 1U) * (source.height() - 1U);
    product.cell_slope_radians_.resize(cell_count);
    product.navigation_passable_.resize(cell_count);
    if (water_level.has_value()) product.wet_.resize(cell_count);
    for (std::uint32_t v = 0U; v + 1U < source.height(); ++v) {
        for (std::uint32_t u = 0U; u + 1U < source.width(); ++u) {
            detail::TerrainTileBuilder::update_cell(product, source, u, v);
        }
    }
    if (auto result = product.validate(); !result) {
        return core::Result<TerrainTileProduct>::failure(result.error());
    }
    return core::Result<TerrainTileProduct>::success(std::move(product));
}

core::Result<TerrainTileProduct> update_terrain_tile(
    const TerrainTileProduct& previous,
    const spatial_field::ScalarField2D& source,
    const spatial_field::DirtyRegion& dirty) {
    if (auto result = previous.validate(); !result) {
        return core::Result<TerrainTileProduct>::failure(result.error());
    }
    if (auto result = validate_source(source, previous.tile_key_); !result) {
        return core::Result<TerrainTileProduct>::failure(result.error());
    }
    const auto current_digest = digest_of(source);
    if (!current_digest) return core::Result<TerrainTileProduct>::failure(current_digest.error());
    if (!dirty.changed || dirty.previous_revision != previous.source_revision_ ||
        dirty.previous_digest != previous.source_digest_ ||
        dirty.resulting_revision != source.source_revision() ||
        dirty.source_digest != current_digest.value() ||
        source.source_revision() != dirty.previous_revision.next() ||
        previous.source_identity_ != source.identity() ||
        previous.source_width_ != source.width() || previous.source_height_ != source.height() ||
        previous.source_origin_uv_.x != source.origin_uv().x ||
        previous.source_origin_uv_.y != source.origin_uv().y ||
        previous.source_cell_size_ != source.cell_size() ||
        dirty.min_u > dirty.max_u || dirty.min_v > dirty.max_v ||
        dirty.max_u >= source.width() || dirty.max_v >= source.height()) {
        return core::Result<TerrainTileProduct>::failure(stale(
            "terrain product does not match the exact source edit receipt"));
    }

    TerrainTileProduct updated = previous;
    const std::uint32_t normal_min_u = dirty.min_u == 0U ? 0U : dirty.min_u - 1U;
    const std::uint32_t normal_min_v = dirty.min_v == 0U ? 0U : dirty.min_v - 1U;
    const std::uint32_t normal_max_u = std::min(dirty.max_u + 1U, source.width() - 1U);
    const std::uint32_t normal_max_v = std::min(dirty.max_v + 1U, source.height() - 1U);
    for (std::size_t mesh_v = 0U; mesh_v < updated.mesh_v_.size(); ++mesh_v) {
        const auto source_v = updated.mesh_v_[mesh_v];
        if (source_v < normal_min_v || source_v > normal_max_v) continue;
        for (std::size_t mesh_u = 0U; mesh_u < updated.mesh_u_.size(); ++mesh_u) {
            const auto source_u = updated.mesh_u_[mesh_u];
            if (source_u < normal_min_u || source_u > normal_max_u) continue;
            auto& vertex = updated.vertices_[mesh_v * updated.mesh_u_.size() + mesh_u];
            vertex.position.y = source.values()[cell_index(source_u, source_v, source.width())];
            vertex.normal = normal_at(source, source_u, source_v);
        }
    }
    const std::uint32_t first_cell_u = dirty.min_u == 0U ? 0U : dirty.min_u - 1U;
    const std::uint32_t first_cell_v = dirty.min_v == 0U ? 0U : dirty.min_v - 1U;
    const std::uint32_t last_cell_u = std::min(dirty.max_u, source.width() - 2U);
    const std::uint32_t last_cell_v = std::min(dirty.max_v, source.height() - 2U);
    for (std::uint32_t v = first_cell_v; v <= last_cell_v; ++v) {
        for (std::uint32_t u = first_cell_u; u <= last_cell_u; ++u) {
            detail::TerrainTileBuilder::update_cell(updated, source, u, v);
        }
    }
    if (auto result = detail::TerrainTileBuilder::update_metadata(updated, source); !result) {
        return core::Result<TerrainTileProduct>::failure(result.error());
    }
    if (auto result = updated.validate(); !result) {
        return core::Result<TerrainTileProduct>::failure(result.error());
    }
    return core::Result<TerrainTileProduct>::success(std::move(updated));
}

} // namespace carto::terrain
