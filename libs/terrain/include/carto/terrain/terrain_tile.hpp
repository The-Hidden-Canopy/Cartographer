#pragma once

#include <carto/hydro/bathymetry.hpp>
#include <carto/spatial_field/scalar_field.hpp>

#include <cstdint>
#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::terrain {

namespace detail {
struct TerrainTileBuilder;
}

struct TileKey {
    std::int32_t x = 0;
    std::int32_t z = 0;
    [[nodiscard]] constexpr auto operator<=>(const TileKey&) const noexcept = default;
};

struct TerrainVertex {
    core::Vec3d position;
    core::Vec3d normal;
};

// A disposable, source-bound derivation. ScalarField2D remains authoritative;
// the mesh and analysis arrays must be rebuilt when source provenance changes.
class TerrainTileProduct final {
public:
    TerrainTileProduct(const TerrainTileProduct&) = default;
    TerrainTileProduct(TerrainTileProduct&&) noexcept = default;
    TerrainTileProduct& operator=(const TerrainTileProduct&) = default;
    TerrainTileProduct& operator=(TerrainTileProduct&&) noexcept = default;

    [[nodiscard]] const std::string& source_identity() const noexcept { return source_identity_; }
    [[nodiscard]] core::Revision source_revision() const noexcept { return source_revision_; }
    [[nodiscard]] const assets::Sha256Digest& source_digest() const noexcept { return source_digest_; }
    [[nodiscard]] TileKey tile_key() const noexcept { return tile_key_; }
    [[nodiscard]] core::Vec2d source_origin_uv() const noexcept { return source_origin_uv_; }
    [[nodiscard]] double source_cell_size() const noexcept { return source_cell_size_; }
    [[nodiscard]] std::uint32_t source_width() const noexcept { return source_width_; }
    [[nodiscard]] std::uint32_t source_height() const noexcept { return source_height_; }
    [[nodiscard]] std::uint32_t lod_step() const noexcept { return lod_step_; }
    [[nodiscard]] double max_navigation_slope_radians() const noexcept {
        return max_navigation_slope_radians_;
    }
    [[nodiscard]] const std::optional<double>& water_level() const noexcept { return water_level_; }
    [[nodiscard]] const std::vector<std::uint32_t>& mesh_u() const noexcept { return mesh_u_; }
    [[nodiscard]] const std::vector<std::uint32_t>& mesh_v() const noexcept { return mesh_v_; }
    [[nodiscard]] const std::vector<TerrainVertex>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<std::uint32_t>& indices() const noexcept { return indices_; }
    [[nodiscard]] const std::vector<double>& cell_slope_radians() const noexcept {
        return cell_slope_radians_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& navigation_passable() const noexcept {
        return navigation_passable_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& wet() const noexcept { return wet_; }
    [[nodiscard]] core::Result<void> validate() const;

private:
    TerrainTileProduct() = default;
    friend struct detail::TerrainTileBuilder;
    friend core::Result<TerrainTileProduct> build_terrain_tile(
        const spatial_field::ScalarField2D&, TileKey, std::uint32_t, double,
        std::optional<double>);
    friend core::Result<TerrainTileProduct> update_terrain_tile(
        const TerrainTileProduct&, const spatial_field::ScalarField2D&,
        const spatial_field::DirtyRegion&);

    std::string source_identity_;
    core::Revision source_revision_;
    assets::Sha256Digest source_digest_;
    TileKey tile_key_;
    core::Vec2d source_origin_uv_;
    double source_cell_size_ = 1.0;
    std::uint32_t source_width_ = 0U;
    std::uint32_t source_height_ = 0U;
    std::uint32_t lod_step_ = 1U;
    double max_navigation_slope_radians_ = 0.0;
    std::optional<double> water_level_;
    std::vector<std::uint32_t> mesh_u_;
    std::vector<std::uint32_t> mesh_v_;
    std::vector<TerrainVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<double> cell_slope_radians_;
    std::vector<std::uint8_t> navigation_passable_;
    std::vector<std::uint8_t> wet_;
};

[[nodiscard]] std::string terrain_tile_identity(TileKey key);
[[nodiscard]] core::Result<TileKey> parse_terrain_tile_identity(std::string_view identity);

// Converts only an authored elevation source to the existing hydro
// bathymetry contract. The coordinate-frame label is mandatory because the
// U/V axes are caller-defined and must not be guessed as world XY.
[[nodiscard]] core::Result<hydro::BathymetryField> to_bathymetry_field(
    const spatial_field::ScalarField2D& source,
    std::string coordinate_frame);

[[nodiscard]] core::Result<TerrainTileProduct> build_terrain_tile(
    const spatial_field::ScalarField2D& source,
    TileKey key,
    std::uint32_t lod_step = 1U,
    double max_navigation_slope_degrees = 35.0,
    std::optional<double> water_level = std::nullopt);

// Updates only the mesh vertices and analysis cells intersecting a truthful
// scalar-field dirty receipt. Any broken revision/digest link requires a full
// rebuild instead of silently reusing the previous product.
[[nodiscard]] core::Result<TerrainTileProduct> update_terrain_tile(
    const TerrainTileProduct& previous,
    const spatial_field::ScalarField2D& source,
    const spatial_field::DirtyRegion& dirty);

} // namespace carto::terrain
