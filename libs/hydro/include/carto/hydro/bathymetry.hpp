#pragma once

#include <carto/core/result.hpp>
#include <carto/hydro/contact.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::hydro {

struct BathymetryField final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U * 1024U;
    static constexpr std::size_t kMaxSamples = 1'048'576U;
    static constexpr std::size_t kMaxTerrainIdentityBytes = 256U;

    HydroSurfaceSource source;
    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    core::Vec2d origin_xy{};
    double cell_size_m = 0.0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    // Samples are row-major, with the lower-left cell at index zero. The
    // sample is a bed elevation, not a water surface or collision mesh.
    std::vector<double> bed_elevations_m;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<double> sample_bed(core::Vec2d point_xy) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<BathymetryField> deserialize(
        std::string_view text);
};

struct WetDryField final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxSamples = BathymetryField::kMaxSamples;

    HydroSurfaceSource source;
    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    core::Vec2d origin_xy{};
    double cell_size_m = 0.0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double water_level_m = 0.0;
    std::vector<std::uint8_t> wet_cells;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_against(
        const BathymetryField& bathymetry) const;
    [[nodiscard]] bool is_wet(std::uint32_t x, std::uint32_t y) const noexcept;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<WetDryField> deserialize(std::string_view text);
};

struct ShorelineSegment final {
    std::uint64_t identity = 0U;
    std::uint64_t wet_cell_id = 0U;
    std::uint8_t side = 0U;
    core::Vec2d start_xy{};
    core::Vec2d end_xy{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct ShorelineField final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxSegments = BathymetryField::kMaxSamples * 4U;

    HydroSurfaceSource source;
    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    core::Vec2d origin_xy{};
    double cell_size_m = 0.0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double water_level_m = 0.0;
    std::vector<ShorelineSegment> segments;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_against(
        const BathymetryField& bathymetry,
        const WetDryField& wet_dry) const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ShorelineField> deserialize(
        std::string_view text);
};

[[nodiscard]] core::Result<WetDryField> derive_wet_dry(
    const BathymetryField& bathymetry,
    double water_level_m);

[[nodiscard]] core::Result<ShorelineField> derive_shoreline(
    const BathymetryField& bathymetry,
    const WetDryField& wet_dry);

} // namespace carto::hydro
