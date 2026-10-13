#include <carto/spatial_field/scalar_field.hpp>
#include <carto/terrain/terrain_tile.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using namespace carto;

static_assert(std::is_const_v<std::remove_reference_t<decltype(
    std::declval<const terrain::TerrainTileProduct&>().vertices())>>,
    "derived terrain arrays must be immutable through the public API");

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

spatial_field::ScalarField2D flat_tile() {
    auto created = spatial_field::ScalarField2D::create(
        terrain::terrain_tile_identity({-2, 7}),
        spatial_field::FieldSemantic::terrain_elevation,
        {100.0, -40.0}, 1.0, 5U, 5U, 0.0, 10.0, 0.0);
    require(static_cast<bool>(created), "canonical terrain source creates");
    return std::move(created.value());
}

void tile_identities_are_canonical_and_bounded() {
    const terrain::TileKey expected{-2, 7};
    const auto identity = terrain::terrain_tile_identity(expected);
    const auto parsed = terrain::parse_terrain_tile_identity(identity);
    require(parsed && parsed.value() == expected,
        "signed terrain tile coordinates round-trip canonically");
    require(!terrain::parse_terrain_tile_identity("terrain.tile.x-02.z7") &&
            !terrain::parse_terrain_tile_identity("terrain.tile.x2147483648.z0") &&
            !terrain::parse_terrain_tile_identity("terrain.tile.x0.z") &&
            !terrain::parse_terrain_tile_identity("terrain.tile.x0.z0.extra"),
        "non-canonical, overflowing, and malformed tile identities fail closed");
}

void terrain_derivation_and_local_rebuild_preserve_provenance() {
    auto source = flat_tile();
    const auto encoded = source.serialize();
    const auto reopened = spatial_field::ScalarField2D::deserialize(encoded);
    require(reopened && reopened.value().semantic() == spatial_field::FieldSemantic::terrain_elevation,
        "appended terrain semantic survives canonical project-source round-trip");

    auto bathymetry = terrain::to_bathymetry_field(source, "condition.pine-ridge.xz.v1");
    require(bathymetry && bathymetry.value().validate() &&
            bathymetry.value().terrain_identity == source.identity() &&
            bathymetry.value().terrain_revision == source.source_revision() &&
            bathymetry.value().terrain_digest == source.canonical_digest().value() &&
            bathymetry.value().width == 4U && bathymetry.value().height == 4U,
        "hydro adapter preserves the exact source identity, revision, and digest");
    const auto hydro_wet_dry = hydro::derive_wet_dry(bathymetry.value(), 0.5);
    require(hydro_wet_dry && hydro_wet_dry.value().validate_against(bathymetry.value()) &&
            hydro_wet_dry.value().is_wet(0U, 0U),
        "existing hydro wet/dry derivation accepts bound terrain source data");
    const auto exact_level = hydro::derive_wet_dry(bathymetry.value(), 0.0);
    require(exact_level && !exact_level.value().is_wet(0U, 0U),
        "hydro wet/dry comparison keeps equality on the dry side");

    auto built = terrain::build_terrain_tile(source, {-2, 7}, 2U, 10.0, 0.5);
    require(built && built.value().validate(), "flat source produces a validated derived tile");
    const auto& flat = built.value();
    require(flat.mesh_u() == std::vector<std::uint32_t>{0U, 2U, 4U} &&
            flat.mesh_v() == std::vector<std::uint32_t>{0U, 2U, 4U} &&
            flat.vertices().size() == 9U && flat.indices().size() == 24U,
        "LOD sampling includes both tile boundaries and emits bounded triangles");
    const auto& triangle_origin = flat.vertices()[flat.indices()[0U]].position;
    const auto& triangle_second = flat.vertices()[flat.indices()[1U]].position;
    const auto& triangle_third = flat.vertices()[flat.indices()[2U]].position;
    require(core::dot(core::cross(
                triangle_second - triangle_origin,
                triangle_third - triangle_origin), flat.vertices()[0U].normal) > 0.0,
        "terrain triangle winding agrees with upward-facing normals");
    require(flat.cell_slope_radians().size() == 16U &&
            std::all_of(flat.navigation_passable().begin(), flat.navigation_passable().end(),
                [](std::uint8_t value) { return value == 1U; }) &&
            std::all_of(flat.wet().begin(), flat.wet().end(),
                [](std::uint8_t value) { return value == 1U; }),
        "flat tile derives zero-slope navigation and water-level masks");
    require(hydro_wet_dry.value().wet_cells == flat.wet(),
        "terrain and hydro derive identical cell wet/dry states at the same water level");
    for (const auto& vertex : flat.vertices()) {
        require(std::abs(vertex.normal.x) < 1.0e-12 &&
                std::abs(vertex.normal.y - 1.0) < 1.0e-12 &&
                std::abs(vertex.normal.z) < 1.0e-12,
            "flat terrain normals point upward and are normalized");
    }

    spatial_field::FieldBrush brush;
    brush.expected_revision = source.source_revision();
    brush.center_uv = {102.0, -38.0};
    brush.radius = 0.5;
    brush.value = 10.0;
    const auto dirty = source.paint(brush);
    require(dirty && dirty.value().changed &&
            dirty.value().previous_revision == core::Revision{1U} &&
            dirty.value().previous_digest == flat.source_digest() &&
            dirty.value().resulting_revision == core::Revision{2U} &&
            dirty.value().source_digest == source.canonical_digest().value(),
        "source edit receipt binds both prior and resulting source revisions and digests");
    auto edited_bathymetry = terrain::to_bathymetry_field(source, "condition.pine-ridge.xz.v1");
    require(edited_bathymetry &&
            !hydro_wet_dry.value().validate_against(edited_bathymetry.value()),
        "hydro products bound to the prior terrain revision reject edited source data");

    const auto incrementally_updated = terrain::update_terrain_tile(
        flat, source, dirty.value());
    const auto fully_rebuilt = terrain::build_terrain_tile(source, {-2, 7}, 2U, 10.0, 0.5);
    require(incrementally_updated && fully_rebuilt &&
            incrementally_updated.value().validate() && fully_rebuilt.value().validate(),
        "valid local edit incrementally rebuilds and validates its derived product");
    const auto& incremental = incrementally_updated.value();
    const auto& full = fully_rebuilt.value();
    require(incremental.source_revision() == full.source_revision() &&
            incremental.source_digest() == full.source_digest() &&
            incremental.indices() == full.indices() &&
            incremental.cell_slope_radians().size() == full.cell_slope_radians().size() &&
            incremental.navigation_passable() == full.navigation_passable() &&
            incremental.wet() == full.wet(),
        "incremental terrain product preserves source-bound topology and analysis outputs");
    const auto edited_hydro_wet_dry = hydro::derive_wet_dry(
        edited_bathymetry.value(), 0.5);
    require(edited_hydro_wet_dry && edited_hydro_wet_dry.value().wet_cells == full.wet(),
        "terrain cell mask agrees with the hydro reference after a source edit");
    for (std::size_t index = 0U; index < full.vertices().size(); ++index) {
        const auto& a = incremental.vertices()[index];
        const auto& b = full.vertices()[index];
        require(std::abs(a.position.x - b.position.x) < 1.0e-12 &&
                std::abs(a.position.y - b.position.y) < 1.0e-12 &&
                std::abs(a.position.z - b.position.z) < 1.0e-12 &&
                std::abs(a.normal.x - b.normal.x) < 1.0e-12 &&
                std::abs(a.normal.y - b.normal.y) < 1.0e-12 &&
                std::abs(a.normal.z - b.normal.z) < 1.0e-12,
            "incremental mesh vertices and normals exactly match a full rebuild");
    }
    for (std::size_t index = 0U; index < full.cell_slope_radians().size(); ++index) {
        require(std::abs(incremental.cell_slope_radians()[index] -
                    full.cell_slope_radians()[index]) < 1.0e-12,
            "incremental cell slopes exactly match a full rebuild");
    }
    require(std::any_of(incremental.navigation_passable().begin(),
                incremental.navigation_passable().end(), [](std::uint8_t value) { return value == 0U; }) &&
            std::any_of(incremental.wet().begin(), incremental.wet().end(),
                [](std::uint8_t value) { return value == 0U; }),
        "raised local terrain changes both navigation admissibility and wet/dry products");

    auto without_water = terrain::build_terrain_tile(source, {-2, 7}, 4U);
    require(without_water && without_water.value().mesh_u() ==
                std::vector<std::uint32_t>{0U, 4U} && without_water.value().wet().empty(),
        "coarse LOD includes tile borders and omits an unrequested wet/dry product");
    require(!terrain::build_terrain_tile(source, {-2, 7}, 0U) &&
            !terrain::build_terrain_tile(source, {-2, 7}, 1U, 91.0) &&
            !terrain::build_terrain_tile(source, {-2, 7}, 1U, 35.0,
                std::numeric_limits<double>::quiet_NaN()),
        "invalid LOD, slope policy, and non-finite water level are rejected");

    auto forged = dirty.value();
    forged.previous_digest = assets::sha256(std::span<const std::uint8_t>{});
    require(!terrain::update_terrain_tile(flat, source, forged),
        "incremental derivation rejects a dirty receipt with a mismatched source parent");
    brush.expected_revision = source.source_revision();
    brush.center_uv = {100.0, -40.0};
    brush.value = 5.0;
    const auto second_dirty = source.paint(brush);
    require(second_dirty && !terrain::update_terrain_tile(flat, source, second_dirty.value()),
        "product cannot skip an intervening source revision");
    require(!terrain::update_terrain_tile(incremental, source, dirty.value()),
        "old dirty receipt cannot be replayed against a newer product revision");

    auto non_terrain = spatial_field::ScalarField2D::create(
        terrain::terrain_tile_identity({-2, 7}), spatial_field::FieldSemantic::moisture,
        {0.0, 0.0}, 1.0, 5U, 5U, 0.0, 1.0, 0.0);
    require(non_terrain && !terrain::build_terrain_tile(non_terrain.value(), {-2, 7}),
        "non-elevation fields cannot be presented as terrain sources");
    require(!terrain::build_terrain_tile(source, {0, 0}),
        "source identity cannot be rebound to a different tile key");
    require(!terrain::to_bathymetry_field(source, ""),
        "hydro adapter fails closed when the coordinate frame is omitted");
}

} // namespace

int main() {
    tile_identities_are_canonical_and_bounded();
    terrain_derivation_and_local_rebuild_preserve_provenance();
    std::cout << "terrain tests passed\n";
    return EXIT_SUCCESS;
}
