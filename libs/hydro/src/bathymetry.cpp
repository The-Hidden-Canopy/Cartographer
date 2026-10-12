#include <carto/hydro/bathymetry.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace carto::hydro {
namespace {

constexpr std::string_view kBathymetryMagic = "CARTOGRAPHER_BATHYMETRY_FIELD";
constexpr std::string_view kWetDryMagic = "CARTOGRAPHER_WET_DRY_FIELD";
constexpr std::string_view kShorelineMagic = "CARTOGRAPHER_SHORELINE_FIELD";
constexpr double kMaxCellSizeM = 1.0e6;
constexpr double kMaxElevationM = 1.0e9;

bool same_source(const HydroSurfaceSource& first, const HydroSurfaceSource& second) {
    return first.domain_id == second.domain_id &&
        first.source_revision == second.source_revision &&
        first.source_digest == second.source_digest &&
        first.coordinate_frame == second.coordinate_frame &&
        first.provenance == second.provenance;
}

bool same_grid(
    core::Vec2d first_origin,
    double first_cell_size,
    std::uint32_t first_width,
    std::uint32_t first_height,
    core::Vec2d second_origin,
    double second_cell_size,
    std::uint32_t second_width,
    std::uint32_t second_height) {
    return first_origin.x == second_origin.x && first_origin.y == second_origin.y &&
        first_cell_size == second_cell_size && first_width == second_width &&
        first_height == second_height;
}

core::Result<void> validate_grid(
    core::Vec2d origin,
    double cell_size,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t sample_count) {
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height;
    if (!origin.finite() || !std::isfinite(cell_size) || cell_size <= 0.0 ||
        cell_size > kMaxCellSizeM || width == 0U || height == 0U ||
        expected > BathymetryField::kMaxSamples || sample_count != expected) {
        return core::Result<void>::failure(detail::validation(
            "hydro field grid is invalid or exceeds its bounded sample count"));
    }
    return core::Result<void>::success();
}

core::Result<void> read_source(std::istream& input, HydroSurfaceSource& source) {
    if (!detail::require_token(input, "SOURCE") ||
        !(input >> std::quoted(source.domain_id)) ||
        !detail::read_revision(input, source.source_revision) ||
        !detail::read_digest(input, source.source_digest) ||
        !(input >> std::quoted(source.coordinate_frame)) ||
        !(input >> std::quoted(source.provenance))) {
        return core::Result<void>::failure(detail::parse_error(
            "hydro field source binding is malformed"));
    }
    return source.validate();
}

core::Result<void> read_terrain(
    std::istream& input,
    std::string& identity,
    core::Revision& revision,
    assets::Sha256Digest& digest) {
    if (!detail::require_token(input, "TERRAIN") ||
        !(input >> std::quoted(identity)) ||
        !detail::read_revision(input, revision) ||
        !detail::read_digest(input, digest) ||
        !detail::safe_text(identity, BathymetryField::kMaxTerrainIdentityBytes) ||
        digest.is_zero()) {
        return core::Result<void>::failure(detail::parse_error(
            "hydro field terrain binding is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_terrain(
    std::string_view identity,
    const assets::Sha256Digest& digest) {
    if (!detail::safe_text(identity, BathymetryField::kMaxTerrainIdentityBytes) ||
        digest.is_zero()) {
        return core::Result<void>::failure(detail::validation(
            "hydro field terrain identity or digest is invalid"));
    }
    return core::Result<void>::success();
}

void write_grid(
    std::ostringstream& output,
    core::Vec2d origin,
    double cell_size,
    std::uint32_t width,
    std::uint32_t height) {
    output << "GRID " << detail::number(origin.x) << ' '
           << detail::number(origin.y) << ' ' << detail::number(cell_size) << ' '
           << width << ' ' << height << '\n';
}

core::Result<void> read_grid(
    std::istream& input,
    core::Vec2d& origin,
    double& cell_size,
    std::uint32_t& width,
    std::uint32_t& height) {
    if (!detail::require_token(input, "GRID") ||
        !detail::read_double(input, origin.x) ||
        !detail::read_double(input, origin.y) ||
        !detail::read_double(input, cell_size) ||
        !detail::read_unsigned(input, width) ||
        !detail::read_unsigned(input, height)) {
        return core::Result<void>::failure(detail::parse_error(
            "hydro field grid is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_common_binding(
    const HydroSurfaceSource& source,
    std::string_view terrain_identity,
    const assets::Sha256Digest& terrain_digest,
    core::Revision terrain_revision,
    core::Vec2d origin,
    double cell_size,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t sample_count) {
    if (auto result = source.validate(); !result) return result;
    if (auto result = validate_terrain(terrain_identity, terrain_digest); !result) {
        return result;
    }
    static_cast<void>(terrain_revision);
    return validate_grid(origin, cell_size, width, height, sample_count);
}

} // namespace

core::Result<void> BathymetryField::validate() const {
    if (auto result = validate_common_binding(
            source, terrain_identity, terrain_digest, terrain_revision, origin_xy,
            cell_size_m, width, height, bed_elevations_m.size()); !result) {
        return result;
    }
    for (const double elevation : bed_elevations_m) {
        if (!std::isfinite(elevation) || std::abs(elevation) > kMaxElevationM) {
            return core::Result<void>::failure(detail::validation(
                "bathymetry contains a non-finite or unbounded bed elevation"));
        }
    }
    return core::Result<void>::success();
}

core::Result<double> BathymetryField::sample_bed(core::Vec2d point_xy) const {
    if (auto result = validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (!point_xy.finite()) {
        return core::Result<double>::failure(detail::invalid(
            "bathymetry query point is non-finite"));
    }
    const double width_m = cell_size_m * static_cast<double>(width);
    const double height_m = cell_size_m * static_cast<double>(height);
    const double local_x = point_xy.x - origin_xy.x;
    const double local_y = point_xy.y - origin_xy.y;
    if (local_x < 0.0 || local_y < 0.0 || local_x >= width_m || local_y >= height_m) {
        return core::Result<double>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "bathymetry query is outside the authored field"));
    }
    const auto x = static_cast<std::uint32_t>(std::floor(local_x / cell_size_m));
    const auto y = static_cast<std::uint32_t>(std::floor(local_y / cell_size_m));
    const std::size_t index = static_cast<std::size_t>(y) * width + x;
    return core::Result<double>::success(bed_elevations_m.at(index));
}

std::string BathymetryField::canonical() const {
    std::ostringstream output;
    output << kBathymetryMagic << ' ' << kSchemaVersion << '\n'
           << source.canonical() << '\n'
           << "TERRAIN " << std::quoted(terrain_identity) << ' '
           << terrain_revision.value() << ' ' << terrain_digest.hex() << '\n';
    write_grid(output, origin_xy, cell_size_m, width, height);
    output << "ELEVATIONS " << bed_elevations_m.size();
    for (const double elevation : bed_elevations_m) output << ' ' << detail::number(elevation);
    output << '\n' << "END\n";
    return output.str();
}

std::string BathymetryField::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> BathymetryField::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<BathymetryField> BathymetryField::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<BathymetryField>::failure(detail::validation(
            "bathymetry document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    BathymetryField field;
    std::size_t count = 0U;
    if (!(input >> magic >> version) || magic != kBathymetryMagic) {
        return core::Result<BathymetryField>::failure(detail::parse_error(
            "bathymetry header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<BathymetryField>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported bathymetry version"));
    }
    if (auto result = read_source(input, field.source); !result ||
        !static_cast<bool>(read_terrain(
            input, field.terrain_identity, field.terrain_revision, field.terrain_digest)) ||
        !static_cast<bool>(read_grid(
            input, field.origin_xy, field.cell_size_m, field.width, field.height)) ||
        !detail::require_token(input, "ELEVATIONS") ||
        !detail::read_count(input, kMaxSamples, count) ||
        !validate_grid(field.origin_xy, field.cell_size_m, field.width, field.height, count)) {
        return core::Result<BathymetryField>::failure(detail::parse_error(
            "bathymetry document is malformed"));
    }
    field.bed_elevations_m.resize(count);
    for (double& elevation : field.bed_elevations_m) {
        if (!detail::read_double(input, elevation)) {
            return core::Result<BathymetryField>::failure(detail::parse_error(
                "bathymetry elevation record is malformed"));
        }
    }
    if (!detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<BathymetryField>::failure(detail::parse_error(
            "bathymetry document has trailing or missing terminator data"));
    }
    if (auto result = field.validate(); !result) {
        return core::Result<BathymetryField>::failure(result.error());
    }
    if (field.serialize() != text) {
        return core::Result<BathymetryField>::failure(detail::validation(
            "bathymetry document is not in canonical form"));
    }
    return core::Result<BathymetryField>::success(std::move(field));
}

core::Result<void> WetDryField::validate() const {
    if (auto result = validate_common_binding(
            source, terrain_identity, terrain_digest, terrain_revision, origin_xy,
            cell_size_m, width, height, wet_cells.size()); !result) {
        return result;
    }
    if (!std::isfinite(water_level_m) || std::abs(water_level_m) > kMaxElevationM ||
        !std::all_of(wet_cells.begin(), wet_cells.end(), [](std::uint8_t value) {
            return value <= 1U;
        })) {
        return core::Result<void>::failure(detail::validation(
            "wet/dry field level or occupancy is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> WetDryField::validate_against(
    const BathymetryField& bathymetry) const {
    if (auto result = validate(); !result) return result;
    if (auto result = bathymetry.validate(); !result) return result;
    if (!same_source(source, bathymetry.source) ||
        terrain_identity != bathymetry.terrain_identity ||
        terrain_revision != bathymetry.terrain_revision ||
        terrain_digest != bathymetry.terrain_digest ||
        !same_grid(origin_xy, cell_size_m, width, height, bathymetry.origin_xy,
            bathymetry.cell_size_m, bathymetry.width, bathymetry.height)) {
        return core::Result<void>::failure(detail::stale(
            "wet/dry field is stale for the bathymetry source"));
    }
    return core::Result<void>::success();
}

bool WetDryField::is_wet(std::uint32_t x, std::uint32_t y) const noexcept {
    if (x >= width || y >= height) return false;
    const std::size_t index = static_cast<std::size_t>(y) * width + x;
    return index < wet_cells.size() && wet_cells[index] != 0U;
}

std::string WetDryField::canonical() const {
    std::ostringstream output;
    output << kWetDryMagic << ' ' << kSchemaVersion << '\n'
           << source.canonical() << '\n'
           << "TERRAIN " << std::quoted(terrain_identity) << ' '
           << terrain_revision.value() << ' ' << terrain_digest.hex() << '\n';
    write_grid(output, origin_xy, cell_size_m, width, height);
    output << "WATER_LEVEL " << detail::number(water_level_m) << '\n'
           << "WET " << wet_cells.size();
    for (const std::uint8_t wet : wet_cells) output << ' ' << static_cast<unsigned>(wet);
    output << '\n' << "END\n";
    return output.str();
}

std::string WetDryField::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> WetDryField::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<WetDryField> WetDryField::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<WetDryField>::failure(detail::validation(
            "wet/dry document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    WetDryField field;
    std::size_t count = 0U;
    if (!(input >> magic >> version) || magic != kWetDryMagic) {
        return core::Result<WetDryField>::failure(detail::parse_error(
            "wet/dry header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<WetDryField>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported wet/dry version"));
    }
    if (auto result = read_source(input, field.source); !result ||
        !static_cast<bool>(read_terrain(
            input, field.terrain_identity, field.terrain_revision, field.terrain_digest)) ||
        !static_cast<bool>(read_grid(
            input, field.origin_xy, field.cell_size_m, field.width, field.height)) ||
        !detail::require_token(input, "WATER_LEVEL") ||
        !detail::read_double(input, field.water_level_m) ||
        !detail::require_token(input, "WET") ||
        !detail::read_count(input, kMaxSamples, count) ||
        !validate_grid(field.origin_xy, field.cell_size_m, field.width, field.height, count)) {
        return core::Result<WetDryField>::failure(detail::parse_error(
            "wet/dry document is malformed"));
    }
    field.wet_cells.resize(count);
    for (std::uint8_t& wet : field.wet_cells) {
        unsigned encoded = 0U;
        if (!detail::read_unsigned(input, encoded) || encoded > 1U) {
            return core::Result<WetDryField>::failure(detail::parse_error(
                "wet/dry occupancy record is malformed"));
        }
        wet = static_cast<std::uint8_t>(encoded);
    }
    if (!detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<WetDryField>::failure(detail::parse_error(
            "wet/dry document has trailing or missing terminator data"));
    }
    if (auto result = field.validate(); !result) {
        return core::Result<WetDryField>::failure(result.error());
    }
    if (field.serialize() != text) {
        return core::Result<WetDryField>::failure(detail::validation(
            "wet/dry document is not in canonical form"));
    }
    return core::Result<WetDryField>::success(std::move(field));
}

core::Result<void> ShorelineSegment::validate() const {
    if (identity == 0U || wet_cell_id == 0U || side > 3U ||
        !start_xy.finite() || !end_xy.finite() ||
        (start_xy.x == end_xy.x && start_xy.y == end_xy.y)) {
        return core::Result<void>::failure(detail::validation(
            "shoreline segment identity, side, or endpoints are invalid"));
    }
    return core::Result<void>::success();
}

std::string ShorelineSegment::canonical() const {
    std::ostringstream output;
    output << "SEGMENT " << identity << ' ' << wet_cell_id << ' '
           << static_cast<unsigned>(side) << ' ' << detail::number(start_xy.x) << ' '
           << detail::number(start_xy.y) << ' ' << detail::number(end_xy.x) << ' '
           << detail::number(end_xy.y);
    return output.str();
}

core::Result<void> ShorelineField::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (auto result = validate_terrain(terrain_identity, terrain_digest); !result) {
        return result;
    }
    const std::uint64_t expected_samples =
        static_cast<std::uint64_t>(width) * height;
    if (!origin_xy.finite() || !std::isfinite(cell_size_m) || cell_size_m <= 0.0 ||
        cell_size_m > kMaxCellSizeM || width == 0U || height == 0U ||
        expected_samples > BathymetryField::kMaxSamples) {
        return core::Result<void>::failure(detail::validation(
            "shoreline field grid is invalid or exceeds its bounded sample count"));
    }
    if (!std::isfinite(water_level_m) || std::abs(water_level_m) > kMaxElevationM ||
        segments.size() > kMaxSegments ||
        !std::is_sorted(segments.begin(), segments.end(),
            [](const ShorelineSegment& first, const ShorelineSegment& second) {
                return first.identity < second.identity;
            }) ||
        std::adjacent_find(segments.begin(), segments.end(),
            [](const ShorelineSegment& first, const ShorelineSegment& second) {
                return first.identity == second.identity;
            }) != segments.end()) {
        return core::Result<void>::failure(detail::validation(
            "shoreline field level, count, or identity ordering is invalid"));
    }
    for (const ShorelineSegment& segment : segments) {
        if (auto result = segment.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> ShorelineField::validate_against(
    const BathymetryField& bathymetry,
    const WetDryField& wet_dry) const {
    if (auto result = validate(); !result) return result;
    if (auto result = wet_dry.validate_against(bathymetry); !result) return result;
    if (!same_source(source, wet_dry.source) ||
        terrain_identity != wet_dry.terrain_identity ||
        terrain_revision != wet_dry.terrain_revision ||
        terrain_digest != wet_dry.terrain_digest ||
        water_level_m != wet_dry.water_level_m ||
        !same_grid(origin_xy, cell_size_m, width, height, wet_dry.origin_xy,
            wet_dry.cell_size_m, wet_dry.width, wet_dry.height)) {
        return core::Result<void>::failure(detail::stale(
            "shoreline field is stale for its bathymetry or wet/dry source"));
    }
    return core::Result<void>::success();
}

std::string ShorelineField::canonical() const {
    std::ostringstream output;
    output << kShorelineMagic << ' ' << kSchemaVersion << '\n'
           << source.canonical() << '\n'
           << "TERRAIN " << std::quoted(terrain_identity) << ' '
           << terrain_revision.value() << ' ' << terrain_digest.hex() << '\n';
    write_grid(output, origin_xy, cell_size_m, width, height);
    output << "WATER_LEVEL " << detail::number(water_level_m) << '\n'
           << "SEGMENTS " << segments.size() << '\n';
    for (const ShorelineSegment& segment : segments) output << segment.canonical() << '\n';
    output << "END\n";
    return output.str();
}

std::string ShorelineField::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> ShorelineField::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<ShorelineField> ShorelineField::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<ShorelineField>::failure(detail::validation(
            "shoreline document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    ShorelineField field;
    std::size_t count = 0U;
    if (!(input >> magic >> version) || magic != kShorelineMagic) {
        return core::Result<ShorelineField>::failure(detail::parse_error(
            "shoreline header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<ShorelineField>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported shoreline version"));
    }
    if (auto result = read_source(input, field.source); !result ||
        !static_cast<bool>(read_terrain(
            input, field.terrain_identity, field.terrain_revision, field.terrain_digest)) ||
        !static_cast<bool>(read_grid(
            input, field.origin_xy, field.cell_size_m, field.width, field.height)) ||
        !detail::require_token(input, "WATER_LEVEL") ||
        !detail::read_double(input, field.water_level_m) ||
        !detail::require_token(input, "SEGMENTS") ||
        !detail::read_count(input, kMaxSegments, count)) {
        return core::Result<ShorelineField>::failure(detail::parse_error(
            "shoreline document is malformed"));
    }
    field.segments.resize(count);
    for (ShorelineSegment& segment : field.segments) {
        unsigned side = 0U;
        if (!detail::require_token(input, "SEGMENT") ||
            !detail::read_unsigned(input, segment.identity) ||
            !detail::read_unsigned(input, segment.wet_cell_id) ||
            !detail::read_unsigned(input, side) || side > 255U ||
            !detail::read_double(input, segment.start_xy.x) ||
            !detail::read_double(input, segment.start_xy.y) ||
            !detail::read_double(input, segment.end_xy.x) ||
            !detail::read_double(input, segment.end_xy.y)) {
            return core::Result<ShorelineField>::failure(detail::parse_error(
                "shoreline segment record is malformed"));
        }
        segment.side = static_cast<std::uint8_t>(side);
    }
    if (!detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<ShorelineField>::failure(detail::parse_error(
            "shoreline document has trailing or missing terminator data"));
    }
    if (auto result = field.validate(); !result) {
        return core::Result<ShorelineField>::failure(result.error());
    }
    if (field.serialize() != text) {
        return core::Result<ShorelineField>::failure(detail::validation(
            "shoreline document is not in canonical form"));
    }
    return core::Result<ShorelineField>::success(std::move(field));
}

core::Result<WetDryField> derive_wet_dry(
    const BathymetryField& bathymetry,
    double water_level_m) {
    if (auto result = bathymetry.validate(); !result) {
        return core::Result<WetDryField>::failure(result.error());
    }
    if (!std::isfinite(water_level_m) || std::abs(water_level_m) > kMaxElevationM) {
        return core::Result<WetDryField>::failure(detail::invalid(
            "wet/dry water level is non-finite or unbounded"));
    }
    WetDryField field;
    field.source = bathymetry.source;
    field.terrain_identity = bathymetry.terrain_identity;
    field.terrain_revision = bathymetry.terrain_revision;
    field.terrain_digest = bathymetry.terrain_digest;
    field.origin_xy = bathymetry.origin_xy;
    field.cell_size_m = bathymetry.cell_size_m;
    field.width = bathymetry.width;
    field.height = bathymetry.height;
    field.water_level_m = water_level_m;
    field.wet_cells.resize(bathymetry.bed_elevations_m.size());
    for (std::size_t index = 0U; index < field.wet_cells.size(); ++index) {
        field.wet_cells[index] =
            bathymetry.bed_elevations_m[index] < water_level_m ? 1U : 0U;
    }
    return core::Result<WetDryField>::success(std::move(field));
}

core::Result<ShorelineField> derive_shoreline(
    const BathymetryField& bathymetry,
    const WetDryField& wet_dry) {
    if (auto result = wet_dry.validate_against(bathymetry); !result) {
        return core::Result<ShorelineField>::failure(result.error());
    }
    ShorelineField field;
    field.source = wet_dry.source;
    field.terrain_identity = wet_dry.terrain_identity;
    field.terrain_revision = wet_dry.terrain_revision;
    field.terrain_digest = wet_dry.terrain_digest;
    field.origin_xy = wet_dry.origin_xy;
    field.cell_size_m = wet_dry.cell_size_m;
    field.width = wet_dry.width;
    field.height = wet_dry.height;
    field.water_level_m = wet_dry.water_level_m;
    const auto add_segment = [&field, &wet_dry](
                                std::uint32_t x,
                                std::uint32_t y,
                                std::uint8_t side,
                                core::Vec2d start,
                                core::Vec2d end) {
        const std::uint64_t cell_id =
            static_cast<std::uint64_t>(y) * wet_dry.width + x + 1U;
        field.segments.push_back(ShorelineSegment{
            cell_id * 4U + static_cast<std::uint64_t>(side) + 1U,
            cell_id,
            side,
            start,
            end});
    };
    for (std::uint32_t y = 0U; y < wet_dry.height; ++y) {
        for (std::uint32_t x = 0U; x < wet_dry.width; ++x) {
            if (!wet_dry.is_wet(x, y)) continue;
            const double left = wet_dry.origin_xy.x + static_cast<double>(x) * wet_dry.cell_size_m;
            const double bottom = wet_dry.origin_xy.y + static_cast<double>(y) * wet_dry.cell_size_m;
            const double right = left + wet_dry.cell_size_m;
            const double top = bottom + wet_dry.cell_size_m;
            if (x == 0U || !wet_dry.is_wet(x - 1U, y)) {
                add_segment(x, y, 0U, {left, bottom}, {left, top});
            }
            if (x + 1U == wet_dry.width || !wet_dry.is_wet(x + 1U, y)) {
                add_segment(x, y, 1U, {right, top}, {right, bottom});
            }
            if (y == 0U || !wet_dry.is_wet(x, y - 1U)) {
                add_segment(x, y, 2U, {right, bottom}, {left, bottom});
            }
            if (y + 1U == wet_dry.height || !wet_dry.is_wet(x, y + 1U)) {
                add_segment(x, y, 3U, {left, top}, {right, top});
            }
        }
    }
    std::sort(field.segments.begin(), field.segments.end(),
        [](const ShorelineSegment& first, const ShorelineSegment& second) {
            return first.identity < second.identity;
        });
    if (auto result = field.validate(); !result) {
        return core::Result<ShorelineField>::failure(result.error());
    }
    return core::Result<ShorelineField>::success(std::move(field));
}

} // namespace carto::hydro
