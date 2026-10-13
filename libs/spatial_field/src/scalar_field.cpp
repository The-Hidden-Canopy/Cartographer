#include <carto/spatial_field/scalar_field.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::spatial_field {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_SCALAR_FIELD_2D";
constexpr double kMaxCoordinate = 1.0e9;
constexpr double kMaxValueMagnitude = 1.0e9;
constexpr double kMinimumCellSize = 1.0e-12;

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic validation(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

bool expect(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::size_t index_of(std::uint32_t u, std::uint32_t v, std::uint32_t width) {
    return static_cast<std::size_t>(v) * width + u;
}

} // namespace

std::string_view field_semantic_name(FieldSemantic semantic) noexcept {
    switch (semantic) {
    case FieldSemantic::generic: return "generic";
    case FieldSemantic::moisture: return "moisture";
    case FieldSemantic::sunlight: return "sunlight";
    case FieldSemantic::shade: return "shade";
    case FieldSemantic::disturbance: return "disturbance";
    case FieldSemantic::wind_u: return "wind_u";
    case FieldSemantic::wind_v: return "wind_v";
    case FieldSemantic::drainage_flow: return "drainage_flow";
    case FieldSemantic::acoustic_dampening: return "acoustic_dampening";
    case FieldSemantic::fog_tendency: return "fog_tendency";
    case FieldSemantic::vegetation_suitability: return "vegetation_suitability";
    case FieldSemantic::terrain_elevation: return "terrain_elevation";
    case FieldSemantic::ridge: return "ridge";
    case FieldSemantic::basin: return "basin";
    case FieldSemantic::drainage: return "drainage";
    case FieldSemantic::low_ground: return "low_ground";
    case FieldSemantic::elevation_band: return "elevation_band";
    }
    return "unknown";
}

core::Result<void> FieldBrush::validate() const {
    if (expected_revision.exhausted() || !std::isfinite(center_uv.x) ||
        !std::isfinite(center_uv.y) || std::abs(center_uv.x) > kMaxCoordinate ||
        std::abs(center_uv.y) > kMaxCoordinate || !std::isfinite(radius) ||
        radius <= 0.0 || radius > kMaxCoordinate || !std::isfinite(value) ||
        std::abs(value) > kMaxValueMagnitude || !std::isfinite(strength) ||
        strength <= 0.0 || strength > 1.0 ||
        static_cast<unsigned>(mode) > static_cast<unsigned>(BrushMode::smooth)) {
        return core::Result<void>::failure(invalid("spatial field brush is invalid or unbounded"));
    }
    return core::Result<void>::success();
}

bool ScalarField2D::same_content(const ScalarField2D& other) const noexcept {
    return identity_ == other.identity_ && semantic_ == other.semantic_ &&
        origin_uv_.x == other.origin_uv_.x && origin_uv_.y == other.origin_uv_.y &&
        cell_size_ == other.cell_size_ && width_ == other.width_ && height_ == other.height_ &&
        minimum_value_ == other.minimum_value_ && maximum_value_ == other.maximum_value_ &&
        values_ == other.values_;
}

core::Result<ScalarField2D> ScalarField2D::create(
    std::string identity,
    FieldSemantic semantic,
    core::Vec2d origin_uv,
    double cell_size,
    std::uint32_t width,
    std::uint32_t height,
    double minimum_value,
    double maximum_value,
    double initial_value) {
    ScalarField2D field;
    field.identity_ = std::move(identity);
    field.semantic_ = semantic;
    field.origin_uv_ = origin_uv;
    field.cell_size_ = cell_size;
    field.width_ = width;
    field.height_ = height;
    field.minimum_value_ = minimum_value;
    field.maximum_value_ = maximum_value;
    if (width > 0U && height > 0U &&
        static_cast<std::size_t>(width) <= kMaxSamples / static_cast<std::size_t>(height)) {
        field.values_.assign(static_cast<std::size_t>(width) * height, initial_value);
    }
    if (auto result = field.validate(); !result) {
        return core::Result<ScalarField2D>::failure(result.error());
    }
    return core::Result<ScalarField2D>::success(std::move(field));
}

core::Result<void> ScalarField2D::validate_header() const {
    const auto sample_count = static_cast<std::uint64_t>(width_) * height_;
    if (!safe_identity(identity_) ||
        static_cast<unsigned>(semantic_) >
            static_cast<unsigned>(FieldSemantic::elevation_band) ||
        !std::isfinite(origin_uv_.x) || !std::isfinite(origin_uv_.y) ||
        std::abs(origin_uv_.x) > kMaxCoordinate || std::abs(origin_uv_.y) > kMaxCoordinate ||
        !std::isfinite(cell_size_) || cell_size_ < kMinimumCellSize ||
        cell_size_ > kMaxCoordinate ||
        width_ < 2U || width_ > kMaxAxisSamples || height_ < 2U ||
        height_ > kMaxAxisSamples || sample_count > kMaxSamples ||
        values_.size() != sample_count || !std::isfinite(minimum_value_) ||
        !std::isfinite(maximum_value_) || minimum_value_ >= maximum_value_ ||
        std::abs(minimum_value_) > kMaxValueMagnitude ||
        std::abs(maximum_value_) > kMaxValueMagnitude || source_revision_.exhausted()) {
        return core::Result<void>::failure(invalid(
            "spatial field identity, grid, range, or revision is invalid"));
    }
    const double extent_u = cell_size_ * static_cast<double>(width_ - 1U);
    const double extent_v = cell_size_ * static_cast<double>(height_ - 1U);
    if (!std::isfinite(extent_u) || !std::isfinite(extent_v) ||
        std::abs(origin_uv_.x + extent_u) > kMaxCoordinate ||
        std::abs(origin_uv_.y + extent_v) > kMaxCoordinate) {
        return core::Result<void>::failure(invalid("spatial field grid extent is out of range"));
    }
    return core::Result<void>::success();
}

core::Result<void> ScalarField2D::validate() const {
    if (auto result = validate_header(); !result) return result;
    for (const double value : values_) {
        if (!std::isfinite(value) || value < minimum_value_ || value > maximum_value_) {
            return core::Result<void>::failure(validation(
                "spatial field sample is non-finite or outside its declared range"));
        }
    }
    return core::Result<void>::success();
}

core::Result<double> ScalarField2D::sample_bilinear(core::Vec2d uv) const {
    if (auto result = validate_header(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        return core::Result<double>::failure(invalid("field sample coordinate is non-finite"));
    }
    const double grid_u = (uv.x - origin_uv_.x) / cell_size_;
    const double grid_v = (uv.y - origin_uv_.y) / cell_size_;
    if (grid_u < 0.0 || grid_v < 0.0 || grid_u > width_ - 1U || grid_v > height_ - 1U) {
        return core::Result<double>::failure({
            core::ErrorCode::not_found, "field sample coordinate lies outside the authored grid"});
    }
    const auto u0 = static_cast<std::uint32_t>(std::floor(grid_u));
    const auto v0 = static_cast<std::uint32_t>(std::floor(grid_v));
    const auto u1 = std::min(u0 + 1U, width_ - 1U);
    const auto v1 = std::min(v0 + 1U, height_ - 1U);
    const std::array<std::size_t, 4U> sample_indices{
        index_of(u0, v0, width_), index_of(u1, v0, width_),
        index_of(u0, v1, width_), index_of(u1, v1, width_)};
    for (const std::size_t index : sample_indices) {
        if (!std::isfinite(values_[index]) || values_[index] < minimum_value_ ||
            values_[index] > maximum_value_) {
            return core::Result<double>::failure(validation(
                "sampled field value is outside its declared range"));
        }
    }
    const double fu = grid_u - u0;
    const double fv = grid_v - v0;
    const double a = values_[index_of(u0, v0, width_)] * (1.0 - fu) +
        values_[index_of(u1, v0, width_)] * fu;
    const double b = values_[index_of(u0, v1, width_)] * (1.0 - fu) +
        values_[index_of(u1, v1, width_)] * fu;
    return core::Result<double>::success(a * (1.0 - fv) + b * fv);
}

core::Result<ScalarField2D> ScalarField2D::at_next_revision(
    core::Revision current_revision) const {
    if (current_revision.exhausted()) {
        return core::Result<ScalarField2D>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "spatial field source revision space is exhausted"));
    }
    ScalarField2D revised = *this;
    revised.source_revision_ = current_revision.next();
    if (auto result = revised.validate(); !result) {
        return core::Result<ScalarField2D>::failure(result.error());
    }
    return core::Result<ScalarField2D>::success(std::move(revised));
}

core::Result<DirtyRegion> ScalarField2D::paint(const FieldBrush& brush) {
    if (auto result = validate(); !result) {
        return core::Result<DirtyRegion>::failure(result.error());
    }
    if (auto result = brush.validate(); !result) {
        return core::Result<DirtyRegion>::failure(result.error());
    }
    if (brush.expected_revision != source_revision_) {
        return core::Result<DirtyRegion>::failure({
            core::ErrorCode::stale_data, "spatial field brush was based on a stale source revision"});
    }
    if (source_revision_.exhausted()) {
        return core::Result<DirtyRegion>::failure({
            core::ErrorCode::invalid_state, "spatial field revision space is exhausted"});
    }

    const std::vector<double> smoothing_source =
        brush.mode == BrushMode::smooth ? values_ : std::vector<double>{};
    const auto previous_digest = canonical_digest();
    if (!previous_digest) return core::Result<DirtyRegion>::failure(previous_digest.error());
    DirtyRegion dirty;
    dirty.min_u = width_;
    dirty.min_v = height_;
    dirty.previous_revision = source_revision_;
    dirty.previous_digest = previous_digest.value();
    dirty.resulting_revision = source_revision_;
    const double inverse_cell = 1.0 / cell_size_;
    const double center_u = (brush.center_uv.x - origin_uv_.x) * inverse_cell;
    const double center_v = (brush.center_uv.y - origin_uv_.y) * inverse_cell;
    const double radius_cells = brush.radius * inverse_cell;
    if (!std::isfinite(center_u) || !std::isfinite(center_v) ||
        !std::isfinite(radius_cells) || radius_cells <= 0.0) {
        return core::Result<DirtyRegion>::failure(invalid(
            "spatial field brush cannot be represented in grid coordinates"));
    }
    const auto floor_clamped = [](double value, std::uint32_t maximum) {
        return static_cast<std::uint32_t>(std::clamp(std::floor(value), 0.0,
            static_cast<double>(maximum)));
    };
    const auto ceil_clamped = [](double value, std::uint32_t maximum) {
        return static_cast<std::uint32_t>(std::clamp(std::ceil(value), 0.0,
            static_cast<double>(maximum)));
    };
    const auto min_u = floor_clamped(center_u - radius_cells, width_ - 1U);
    const auto min_v = floor_clamped(center_v - radius_cells, height_ - 1U);
    const auto max_u = ceil_clamped(center_u + radius_cells, width_ - 1U);
    const auto max_v = ceil_clamped(center_v + radius_cells, height_ - 1U);

    for (std::uint32_t v = min_v; v <= max_v; ++v) {
        for (std::uint32_t u = min_u; u <= max_u; ++u) {
            const double du = static_cast<double>(u) - center_u;
            const double dv = static_cast<double>(v) - center_v;
            const double normalized_distance = std::sqrt(du * du + dv * dv) / radius_cells;
            if (normalized_distance >= 1.0) continue;
            const double influence = brush.strength * (1.0 - normalized_distance);
            const auto sample_index = index_of(u, v, width_);
            const double before = values_[sample_index];
            double after = before;
            switch (brush.mode) {
            case BrushMode::replace:
                after = before + (brush.value - before) * influence;
                break;
            case BrushMode::add:
                after = before + brush.value * influence;
                break;
            case BrushMode::erase:
                after = before + (minimum_value_ - before) * influence;
                break;
            case BrushMode::smooth: {
                double total = 0.0;
                std::uint32_t neighbors = 0U;
                const std::uint32_t first_u = u == 0U ? 0U : u - 1U;
                const std::uint32_t first_v = v == 0U ? 0U : v - 1U;
                const std::uint32_t last_u = std::min(u + 1U, width_ - 1U);
                const std::uint32_t last_v = std::min(v + 1U, height_ - 1U);
                for (std::uint32_t nv = first_v; nv <= last_v; ++nv) {
                    for (std::uint32_t nu = first_u; nu <= last_u; ++nu) {
                        total += smoothing_source[index_of(nu, nv, width_)];
                        ++neighbors;
                    }
                }
                const double average = total / neighbors;
                after = before + (average - before) * influence;
                break;
            }
            }
            after = std::clamp(after, minimum_value_, maximum_value_);
            if (after == before) continue;
            values_[sample_index] = after;
            dirty.min_u = std::min(dirty.min_u, u);
            dirty.min_v = std::min(dirty.min_v, v);
            dirty.max_u = std::max(dirty.max_u, u);
            dirty.max_v = std::max(dirty.max_v, v);
            dirty.changed = true;
        }
    }

    if (dirty.changed) source_revision_ = source_revision_.next();
    dirty.resulting_revision = source_revision_;
    const auto digest = canonical_digest();
    if (!digest) return core::Result<DirtyRegion>::failure(digest.error());
    dirty.source_digest = digest.value();
    return core::Result<DirtyRegion>::success(dirty);
}

std::string ScalarField2D::serialize() const {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << kMagic << ' ' << kSchemaVersion << '\n';
    out << "SOURCE " << std::quoted(identity_) << ' '
        << static_cast<unsigned>(semantic_) << ' ' << source_revision_.value() << '\n';
    out << "GRID " << origin_uv_.x << ' ' << origin_uv_.y << ' ' << cell_size_ << ' '
        << width_ << ' ' << height_ << ' ' << minimum_value_ << ' ' << maximum_value_ << '\n';
    out << "VALUES " << values_.size() << '\n';
    for (std::size_t index = 0U; index < values_.size(); ++index) {
        out << values_[index] << ((index + 1U) % 8U == 0U ? '\n' : ' ');
    }
    if (values_.size() % 8U != 0U) out << '\n';
    out << "END\n";
    return out.str();
}

core::Result<assets::Sha256Digest> ScalarField2D::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<ScalarField2D> ScalarField2D::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<ScalarField2D>::failure(invalid(
            "spatial field record is empty or exceeds its size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    ScalarField2D field;
    unsigned semantic = 0U;
    std::uint64_t revision = 0U;
    if (!expect(input, kMagic) || !read_unsigned(input, version) ||
        (version != 1U && version != kSchemaVersion) ||
        !expect(input, "SOURCE") ||
        !(input >> std::quoted(field.identity_)) || !read_unsigned(input, semantic) ||
        semantic > static_cast<unsigned>(version == 1U
            ? FieldSemantic::vegetation_suitability : FieldSemantic::elevation_band) ||
        !read_unsigned(input, revision) || !expect(input, "GRID") ||
        !(input >> field.origin_uv_.x >> field.origin_uv_.y >> field.cell_size_) ||
        !read_unsigned(input, field.width_) || !read_unsigned(input, field.height_) ||
        !(input >> field.minimum_value_ >> field.maximum_value_)) {
        return core::Result<ScalarField2D>::failure(validation(
            "spatial field header or grid record is malformed"));
    }
    field.semantic_ = static_cast<FieldSemantic>(semantic);
    field.source_revision_ = core::Revision{revision};
    std::uint64_t count = 0U;
    if (!expect(input, "VALUES") || !read_unsigned(input, count) ||
        count > kMaxSamples || count != static_cast<std::uint64_t>(field.width_) * field.height_) {
        return core::Result<ScalarField2D>::failure(validation(
            "spatial field sample count is invalid"));
    }
    field.values_.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        double value = 0.0;
        if (!(input >> value)) {
            return core::Result<ScalarField2D>::failure(validation(
                "spatial field sample data is truncated"));
        }
        field.values_.push_back(value);
    }
    if (!expect(input, "END")) {
        return core::Result<ScalarField2D>::failure(validation(
            "spatial field terminal record is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ScalarField2D>::failure(validation(
            "spatial field contains unexpected trailing data"));
    }
    if (auto result = field.validate(); !result) {
        return core::Result<ScalarField2D>::failure(result.error());
    }
    return core::Result<ScalarField2D>::success(std::move(field));
}

} // namespace carto::spatial_field
