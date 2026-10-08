#include <carto/scientific/model.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numbers>
#include <set>
#include <sstream>
#include <utility>

namespace carto::scientific {

namespace {

using core::Diagnostic;
using core::ErrorCode;

constexpr std::size_t kMaxCollectionEntries = 100'000U;
constexpr std::size_t kMaxVolumeRaySamples = 2'000'000U;
constexpr std::size_t kMaxTags = 128U;
constexpr std::size_t kMaxReferenceBytes = 16U * 1024U;
constexpr std::size_t kMaxTextBytes = 64U * 1024U;
constexpr std::size_t kMaxParameterSweepAxes = 32U;
constexpr std::size_t kMaxParameterSweepValues = 128U;
constexpr std::size_t kMaxParameterSweepCases = 4'096U;
constexpr std::size_t kMaxParameterSweepParameters = 256U;
constexpr std::size_t kMaxMorphospacePoints = 4'096U;

struct PrincipalAxesAnalysis {
    std::array<core::Vec3d, 3> axes{};
    std::array<double, 3> variances{};
};

// Shared CPU reference implementation for morphology principal axes. The
// caller owns the semantic boundary; this helper only receives finite point
// positions and refuses ambiguous spectra rather than inventing a tie-break.
std::optional<PrincipalAxesAnalysis> compute_principal_axes(
    std::span<const core::Vec3d> positions) {
    if (positions.size() < 3U) return std::nullopt;

    core::Vec3d centroid{};
    for (const auto position : positions) {
        centroid = centroid + position;
        if (!centroid.finite()) return std::nullopt;
    }
    centroid = centroid * (1.0 / static_cast<double>(positions.size()));
    if (!centroid.finite()) return std::nullopt;

    using Matrix3 = std::array<std::array<double, 3>, 3>;
    Matrix3 covariance{};
    for (const auto position : positions) {
        const auto centered = position - centroid;
        if (!centered.finite()) return std::nullopt;
        const std::array<double, 3> values{centered.x, centered.y, centered.z};
        for (std::size_t row = 0U; row < 3U; ++row) {
            for (std::size_t column = row; column < 3U; ++column) {
                covariance[row][column] += values[row] * values[column];
                if (!std::isfinite(covariance[row][column])) return std::nullopt;
                covariance[column][row] = covariance[row][column];
            }
        }
    }
    const double count = static_cast<double>(positions.size());
    double covariance_scale = 0.0;
    for (auto& row : covariance) {
        for (double& value : row) {
            value /= count;
            covariance_scale = std::max(covariance_scale, std::abs(value));
        }
    }
    if (!std::isfinite(covariance_scale) ||
        covariance_scale <= std::numeric_limits<double>::min()) {
        return std::nullopt;
    }

    Matrix3 eigenvectors{{
        {{1.0, 0.0, 0.0}},
        {{0.0, 1.0, 0.0}},
        {{0.0, 0.0, 1.0}},
    }};
    bool converged = false;
    for (std::size_t iteration = 0U; iteration < 64U; ++iteration) {
        std::size_t p = 0U;
        std::size_t q = 1U;
        double largest = std::abs(covariance[0][1]);
        if (std::abs(covariance[0][2]) > largest) {
            p = 0U;
            q = 2U;
            largest = std::abs(covariance[0][2]);
        }
        if (std::abs(covariance[1][2]) > largest) {
            p = 1U;
            q = 2U;
            largest = std::abs(covariance[1][2]);
        }
        if (!std::isfinite(largest)) return std::nullopt;
        if (largest <= 1.0e-14 * covariance_scale) {
            converged = true;
            break;
        }

        const double app = covariance[p][p];
        const double aqq = covariance[q][q];
        const double apq = covariance[p][q];
        const double angle = 0.5 * std::atan2(2.0 * apq, aqq - app);
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        if (!std::isfinite(cosine) || !std::isfinite(sine)) return std::nullopt;
        for (std::size_t index = 0U; index < 3U; ++index) {
            if (index == p || index == q) continue;
            const double index_p = covariance[index][p];
            const double index_q = covariance[index][q];
            covariance[index][p] = covariance[p][index] =
                cosine * index_p - sine * index_q;
            covariance[index][q] = covariance[q][index] =
                sine * index_p + cosine * index_q;
        }
        covariance[p][p] = cosine * cosine * app -
            2.0 * sine * cosine * apq + sine * sine * aqq;
        covariance[q][q] = sine * sine * app +
            2.0 * sine * cosine * apq + cosine * cosine * aqq;
        covariance[p][q] = covariance[q][p] = 0.0;
        for (std::size_t row = 0U; row < 3U; ++row) {
            const double row_p = eigenvectors[row][p];
            const double row_q = eigenvectors[row][q];
            eigenvectors[row][p] = cosine * row_p - sine * row_q;
            eigenvectors[row][q] = sine * row_p + cosine * row_q;
        }
    }
    if (!converged) return std::nullopt;

    std::array<double, 3> eigenvalues{};
    for (std::size_t index = 0U; index < 3U; ++index) {
        eigenvalues[index] = covariance[index][index];
        const double eigenvalue_tolerance = 1.0e-12 * covariance_scale;
        if (!std::isfinite(eigenvalues[index]) ||
            eigenvalues[index] < -eigenvalue_tolerance) {
            return std::nullopt;
        }
        eigenvalues[index] = std::max(0.0, eigenvalues[index]);
    }
    std::array<std::size_t, 3> order{0U, 1U, 2U};
    std::sort(order.begin(), order.end(), [&eigenvalues](std::size_t left,
                                                          std::size_t right) {
        if (eigenvalues[left] != eigenvalues[right]) {
            return eigenvalues[left] > eigenvalues[right];
        }
        return left < right;
    });
    const double eigenvalue_scale = std::max({
        std::abs(eigenvalues[order[0]]),
        std::abs(eigenvalues[order[1]]),
        std::abs(eigenvalues[order[2]])});
    if (!std::isfinite(eigenvalue_scale) ||
        eigenvalue_scale <= std::numeric_limits<double>::min()) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < 2U; ++index) {
        const double gap = eigenvalues[order[index]] - eigenvalues[order[index + 1U]];
        if (!std::isfinite(gap) || gap <= 1.0e-10 * eigenvalue_scale) {
            return std::nullopt;
        }
    }

    std::array<core::Vec3d, 3> axes{};
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        axes[axis] = core::Vec3d{
            eigenvectors[0][order[axis]],
            eigenvectors[1][order[axis]],
            eigenvectors[2][order[axis]],
        }.normalized();
        if (!axes[axis].finite()) return std::nullopt;
    }
    const double handedness = core::dot(axes[0], core::cross(axes[1], axes[2]));
    if (!std::isfinite(handedness) || std::abs(handedness) <= 1.0e-8) {
        return std::nullopt;
    }
    if (handedness < 0.0) axes[2] = axes[2] * -1.0;
    return PrincipalAxesAnalysis{
        axes, {eigenvalues[order[0]], eigenvalues[order[1]], eigenvalues[order[2]]}};
}

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

Diagnostic parse_error(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t limit, bool required) {
    if (value.size() > limit || (required && value.empty())) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

core::Result<void> validate_text(
    std::string_view value,
    std::string_view field,
    std::size_t limit,
    bool required = false) {
    if (!safe_text(value, limit, required)) {
        return core::Result<void>::failure(validation(
            std::string(field) + " is empty, too long, or contains a control byte"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_digest(
    const std::optional<assets::Sha256Digest>& digest,
    std::string_view field) {
    if (digest.has_value() && digest->is_zero()) {
        return core::Result<void>::failure(validation(
            std::string(field) + " must not be a zero digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_id(std::uint64_t value, std::string_view field) {
    if (value == 0U) {
        return core::Result<void>::failure(invalid(std::string(field) + " must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_provenance(const Provenance& provenance) {
    if (auto result = validate_text(provenance.source_reference, "provenance source reference",
                                    kMaxReferenceBytes); !result) return result;
    if (auto result = validate_text(provenance.release, "provenance release", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_text(provenance.license, "provenance license", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_text(provenance.provider, "provenance provider", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_text(provenance.imported_at_utc,
                                    "provenance imported_at_utc", 128U); !result) return result;
    if (!provenance.empty() && provenance.source_reference.empty()) {
        return core::Result<void>::failure(validation(
            "non-empty provenance must include a source reference"));
    }
    if (!provenance.imported_at_utc.empty() && provenance.imported_at_utc.back() != 'Z') {
        return core::Result<void>::failure(validation(
            "provenance imported_at_utc must be an explicit UTC value ending in Z"));
    }
    return validate_digest(provenance.content_digest, "provenance content digest");
}

core::Result<void> validate_geometry(const GeometryBinding& geometry) {
    if (auto result = validate_text(geometry.kind, "geometry binding kind", 128U); !result) {
        return result;
    }
    if (auto result = validate_text(geometry.reference, "geometry binding reference",
                                    kMaxReferenceBytes); !result) return result;
    if (geometry.kind.empty() != geometry.reference.empty()) {
        return core::Result<void>::failure(validation(
            "geometry binding kind and reference must be both present or both absent"));
    }
    return core::Result<void>::success();
}

template <typename Map, typename Value>
core::Result<void> insert_unique(Map& map, Value value, std::string_view label) {
    if (map.size() >= kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            std::string(label) + " collection exceeds the safety limit"));
    }
    if (auto result = value.validate(); !result) return result;
    if (!value.id) {
        return core::Result<void>::failure(invalid(std::string(label) + " id must be non-zero"));
    }
    const auto [iterator, inserted] = map.emplace(value.id, std::move(value));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(std::string(label) + " id is duplicated"));
    }
    return core::Result<void>::success();
}

template <typename IdType>
core::Result<void> validate_id_list(
    const std::vector<IdType>& ids,
    std::string_view field) {
    if (ids.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            std::string(field) + " list exceeds the safety limit"));
    }
    std::set<IdType> unique;
    for (const auto id : ids) {
        if (!id || !unique.insert(id).second) {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains a zero or duplicate id"));
        }
    }
    return core::Result<void>::success();
}

template <typename IdType>
std::uint64_t optional_id(const std::optional<IdType>& id) {
    return id.has_value() ? id->value : 0U;
}

template <typename IdType>
core::Result<std::optional<IdType>> read_optional_id(
    std::istream& input,
    std::string_view field) {
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::optional<IdType>>::failure(parse_error(
            std::string(field) + " is missing"));
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::optional<IdType>>::failure(parse_error(
            std::string(field) + " is not an unsigned integer"));
    }
    if (value == 0U) return core::Result<std::optional<IdType>>::success(std::nullopt);
    return core::Result<std::optional<IdType>>::success(IdType{value});
}

core::Result<std::uint64_t> read_uint(std::istream& input, std::string_view field) {
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(field) + " is missing"));
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(field) + " is not an unsigned integer"));
    }
    return core::Result<std::uint64_t>::success(value);
}

core::Result<double> read_double(std::istream& input, std::string_view field) {
    double value = 0.0;
    if (!(input >> value) || !std::isfinite(value)) {
        return core::Result<double>::failure(parse_error(
            std::string(field) + " is not a finite number"));
    }
    return core::Result<double>::success(value);
}

core::Result<std::string> read_string(
    std::istream& input,
    std::string_view field,
    std::size_t limit,
    bool required = false) {
    std::string value;
    if (!(input >> std::quoted(value))) {
        return core::Result<std::string>::failure(parse_error(
            std::string(field) + " is not a quoted string"));
    }
    if (!safe_text(value, limit, required)) {
        return core::Result<std::string>::failure(parse_error(
            std::string(field) + " is empty, too long, or contains a control byte"));
    }
    return core::Result<std::string>::success(std::move(value));
}

core::Result<void> require_record(std::istream& input, std::string_view expected) {
    std::string record;
    if (!(input >> record) || record != expected) {
        return core::Result<void>::failure(parse_error(
            "scientific model is missing the expected " + std::string(expected) + " record"));
    }
    return core::Result<void>::success();
}

void write_provenance(std::ostream& output, const Provenance& provenance) {
    output << std::quoted(provenance.source_reference) << ' '
           << std::quoted(provenance.release) << ' '
           << std::quoted(provenance.license) << ' '
           << std::quoted(provenance.provider) << ' '
           << std::quoted(provenance.imported_at_utc) << ' '
           << (provenance.content_digest.has_value() ? 1U : 0U);
    if (provenance.content_digest.has_value()) output << ' ' << provenance.content_digest->hex();
}

core::Result<Provenance> read_provenance(std::istream& input) {
    const auto source = read_string(input, "provenance source reference", kMaxReferenceBytes);
    const auto release = read_string(input, "provenance release", kMaxTextBytes);
    const auto license = read_string(input, "provenance license", kMaxTextBytes);
    const auto provider = read_string(input, "provenance provider", kMaxTextBytes);
    const auto imported = read_string(input, "provenance imported_at_utc", 128U);
    if (!source || !release || !license || !provider || !imported) {
        const auto& error = !source ? source.error() : !release ? release.error() :
            !license ? license.error() : !provider ? provider.error() : imported.error();
        return core::Result<Provenance>::failure(error);
    }
    const auto has_digest = read_uint(input, "provenance digest flag");
    if (!has_digest || has_digest.value() > 1U) {
        return core::Result<Provenance>::failure(parse_error(
            "provenance digest flag is invalid"));
    }
    Provenance provenance{source.value(), release.value(), license.value(), provider.value(),
                          imported.value(), std::nullopt};
    if (has_digest.value() != 0U) {
        std::string encoded;
        if (!(input >> encoded)) {
            return core::Result<Provenance>::failure(parse_error(
                "provenance digest is missing"));
        }
        const auto digest = assets::Sha256Digest::from_hex(encoded);
        if (!digest) return core::Result<Provenance>::failure(digest.error());
        provenance.content_digest = digest.value();
    }
    if (auto result = provenance.validate(); !result) {
        return core::Result<Provenance>::failure(result.error());
    }
    return core::Result<Provenance>::success(std::move(provenance));
}

template <typename IdType>
void write_id_list(std::ostream& output, const std::vector<IdType>& ids) {
    output << ids.size();
    for (const auto id : ids) output << ' ' << id.value;
}

template <typename IdType>
core::Result<std::vector<IdType>> read_id_list(
    std::istream& input,
    std::string_view field) {
    const auto count = read_uint(input, std::string(field) + " count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<std::vector<IdType>>::failure(parse_error(
            std::string(field) + " count is invalid or exceeds the safety limit"));
    }
    std::vector<IdType> ids;
    ids.reserve(static_cast<std::size_t>(count.value()));
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        const auto id = read_uint(input, field);
        if (!id || id.value() == 0U) {
            return core::Result<std::vector<IdType>>::failure(parse_error(
                std::string(field) + " contains an invalid id"));
        }
        ids.push_back(IdType{id.value()});
    }
    if (auto result = validate_id_list(ids, field); !result) {
        return core::Result<std::vector<IdType>>::failure(result.error());
    }
    return core::Result<std::vector<IdType>>::success(std::move(ids));
}

void write_double_list(std::ostream& output, const std::vector<double>& values) {
    output << values.size();
    for (const auto value : values) output << ' ' << value;
}

core::Result<std::vector<double>> read_double_list(
    std::istream& input,
    std::string_view field) {
    const auto count = read_uint(input, std::string(field) + " count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<std::vector<double>>::failure(parse_error(
            std::string(field) + " count is invalid or exceeds the safety limit"));
    }
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(count.value()));
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        const auto value = read_double(input, field);
        if (!value) return core::Result<std::vector<double>>::failure(value.error());
        values.push_back(value.value());
    }
    return core::Result<std::vector<double>>::success(std::move(values));
}

void write_optional_digest(
    std::ostream& output,
    const std::optional<assets::Sha256Digest>& digest) {
    output << ' ' << (digest.has_value() ? 1U : 0U);
    if (digest.has_value()) output << ' ' << digest->hex();
}

core::Result<std::optional<assets::Sha256Digest>> read_optional_digest(
    std::istream& input,
    std::string_view field) {
    const auto present = read_uint(input, std::string(field) + " presence");
    if (!present || present.value() > 1U) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(parse_error(
            std::string(field) + " presence flag is invalid"));
    }
    if (present.value() == 0U) {
        return core::Result<std::optional<assets::Sha256Digest>>::success(std::nullopt);
    }
    std::string encoded;
    if (!(input >> encoded)) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(parse_error(
            std::string(field) + " digest is missing"));
    }
    const auto digest = assets::Sha256Digest::from_hex(encoded);
    if (!digest || digest.value().is_zero()) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(parse_error(
            std::string(field) + " digest is invalid"));
    }
    return core::Result<std::optional<assets::Sha256Digest>>::success(digest.value());
}

template <typename IdType, typename Value>
core::Result<void> validate_reference(
    const std::map<IdType, Value>& values,
    IdType id,
    std::string_view field) {
    if (!id || !values.contains(id)) {
        return core::Result<void>::failure(validation(
            std::string(field) + " references an unknown id"));
    }
    return core::Result<void>::success();
}

template <typename IdType, typename Value>
core::Result<void> validate_reference_list(
    const std::map<IdType, Value>& values,
    const std::vector<IdType>& ids,
    std::string_view field) {
    if (auto result = validate_id_list(ids, field); !result) return result;
    for (const auto id : ids) {
        if (!values.contains(id)) {
            return core::Result<void>::failure(validation(
                std::string(field) + " references an unknown id"));
        }
    }
    return core::Result<void>::success();
}

template <typename IdType>
core::Result<void> validate_optional_reference(
    const std::optional<IdType>& id,
    std::string_view field,
    const auto& values) {
    if (id.has_value() && !values.contains(*id)) {
        return core::Result<void>::failure(validation(
            std::string(field) + " references an unknown id"));
    }
    return core::Result<void>::success();
}

} // namespace

std::string_view field_kind_name(FieldKind kind) noexcept {
    switch (kind) {
    case FieldKind::scalar: return "scalar";
    case FieldKind::vector: return "vector";
    case FieldKind::tensor: return "tensor";
    case FieldKind::categorical: return "categorical";
    }
    return "unknown";
}

std::string_view association_name(Association association) noexcept {
    switch (association) {
    case Association::point: return "point";
    case Association::edge: return "edge";
    case Association::face: return "face";
    case Association::cell: return "cell";
    case Association::continuous: return "continuous";
    case Association::sample_set: return "sample_set";
    }
    return "unknown";
}

bool Provenance::empty() const noexcept {
    return source_reference.empty() && release.empty() && license.empty() && provider.empty() &&
        imported_at_utc.empty() && !content_digest.has_value();
}

core::Result<void> Provenance::validate() const {
    return validate_provenance(*this);
}

core::Result<void> GeometryBinding::validate() const {
    return validate_geometry(*this);
}

core::Result<void> Region::validate() const {
    if (auto result = validate_id(id.value, "region id"); !result) return result;
    if (auto result = validate_text(semantic_type, "region semantic type", 256U, true); !result) return result;
    if (auto result = geometry.validate(); !result) return result;
    if (auto result = validate_text(material_or_tissue_binding,
                                    "region material/tissue binding", kMaxReferenceBytes);
        !result) return result;
    if (tags.size() > kMaxTags) {
        return core::Result<void>::failure(validation("region contains too many tags"));
    }
    std::set<std::string> unique;
    for (const auto& tag : tags) {
        if (auto result = validate_text(tag, "region tag", 256U, true); !result) return result;
        if (!unique.insert(tag).second) {
            return core::Result<void>::failure(validation("region contains duplicate tags"));
        }
    }
    return provenance.validate();
}

core::Result<void> Boundary::validate() const {
    if (auto result = validate_id(id.value, "boundary id"); !result) return result;
    if (auto result = validate_text(semantic_type, "boundary semantic type", 256U, true); !result) return result;
    if (auto result = geometry.validate(); !result) return result;
    return provenance.validate();
}

core::Result<void> Interface::validate() const {
    if (auto result = validate_id(id.value, "interface id"); !result) return result;
    if (auto result = validate_text(semantic_type, "interface semantic type", 256U, true); !result) return result;
    if (auto result = validate_id(first_region.value, "interface first region"); !result) return result;
    if (auto result = validate_id(second_region.value, "interface second region"); !result) return result;
    if (first_region == second_region) {
        return core::Result<void>::failure(validation("interface regions must be distinct"));
    }
    return provenance.validate();
}

core::Result<void> Source::validate() const {
    if (auto result = validate_id(id.value, "source id"); !result) return result;
    if (auto result = validate_text(semantic_type, "source semantic type", 256U, true); !result) return result;
    if (auto result = validate_text(units, "source units", 128U); !result) return result;
    if (!std::isfinite(value)) return core::Result<void>::failure(validation("source value is not finite"));
    return provenance.validate();
}

core::Result<void> Load::validate() const {
    if (auto result = validate_id(id.value, "load id"); !result) return result;
    if (auto result = validate_text(semantic_type, "load semantic type", 256U, true); !result) return result;
    if (auto result = validate_text(units, "load units", 128U); !result) return result;
    if (!std::isfinite(value)) return core::Result<void>::failure(validation("load value is not finite"));
    return provenance.validate();
}

core::Result<void> Parameter::validate() const {
    if (auto result = validate_id(id.value, "parameter id"); !result) return result;
    if (auto result = validate_text(name, "parameter name", 256U, true); !result) return result;
    if (auto result = validate_text(units, "parameter units", 128U); !result) return result;
    if (!std::isfinite(value)) return core::Result<void>::failure(validation("parameter value is not finite"));
    if (lower_bound.has_value() && !std::isfinite(*lower_bound)) {
        return core::Result<void>::failure(validation("parameter lower bound is not finite"));
    }
    if (upper_bound.has_value() && !std::isfinite(*upper_bound)) {
        return core::Result<void>::failure(validation("parameter upper bound is not finite"));
    }
    if (lower_bound.has_value() && upper_bound.has_value() && *lower_bound > *upper_bound) {
        return core::Result<void>::failure(validation("parameter bounds are inverted"));
    }
    if (lower_bound.has_value() && value < *lower_bound) {
        return core::Result<void>::failure(validation("parameter value is below its lower bound"));
    }
    if (upper_bound.has_value() && value > *upper_bound) {
        return core::Result<void>::failure(validation("parameter value is above its upper bound"));
    }
    return provenance.validate();
}

core::Result<void> ParameterAssignment::validate() const {
    if (auto result = validate_id(parameter.value, "parameter assignment parameter"); !result) {
        return result;
    }
    if (!std::isfinite(value)) {
        return core::Result<void>::failure(validation(
            "parameter assignment value is not finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> ParameterSweepAxis::validate() const {
    if (auto result = validate_id(parameter.value, "parameter sweep axis parameter"); !result) {
        return result;
    }
    if (values.empty() || values.size() > kMaxParameterSweepValues) {
        return core::Result<void>::failure(validation(
            "parameter sweep axis has an invalid value count"));
    }
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<void>::failure(validation(
                "parameter sweep axis contains a non-finite value"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ParameterSweepPreview::validate() const {
    if (auto result = validate_id(study.value, "parameter sweep study"); !result) return result;
    if (source_model_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "parameter sweep source revision is exhausted"));
    }
    if (auto result = validate_text(strategy, "parameter sweep strategy", 64U, true); !result) {
        return result;
    }
    if (strategy != "cartesian") {
        return core::Result<void>::failure(validation(
            "parameter sweep strategy is unsupported"));
    }
    if (base_assignments.empty() || base_assignments.size() > kMaxParameterSweepParameters ||
        axes.empty() || axes.size() > kMaxParameterSweepAxes ||
        cases.empty() || cases.size() > kMaxParameterSweepCases) {
        return core::Result<void>::failure(validation(
            "parameter sweep preview has an invalid bounded collection"));
    }

    std::set<ParameterId> base_ids;
    for (std::size_t index = 0U; index < base_assignments.size(); ++index) {
        if (auto result = base_assignments[index].validate(); !result) return result;
        if (!base_ids.insert(base_assignments[index].parameter).second) {
            return core::Result<void>::failure(validation(
                "parameter sweep base assignments contain a duplicate parameter"));
        }
        if (index > 0U && !(base_assignments[index - 1U].parameter <
                            base_assignments[index].parameter)) {
            return core::Result<void>::failure(validation(
                "parameter sweep base assignments are not in canonical order"));
        }
    }

    std::set<ParameterId> axis_ids;
    for (std::size_t index = 0U; index < axes.size(); ++index) {
        if (auto result = axes[index].validate(); !result) return result;
        if (!base_ids.contains(axes[index].parameter)) {
            return core::Result<void>::failure(validation(
                "parameter sweep axis references a parameter outside the study"));
        }
        if (!axis_ids.insert(axes[index].parameter).second) {
            return core::Result<void>::failure(validation(
                "parameter sweep contains a duplicate axis"));
        }
        if (index > 0U && !(axes[index - 1U].parameter < axes[index].parameter)) {
            return core::Result<void>::failure(validation(
                "parameter sweep axes are not in canonical order"));
        }
        for (std::size_t value_index = 1U; value_index < axes[index].values.size(); ++value_index) {
            if (!(axes[index].values[value_index - 1U] < axes[index].values[value_index])) {
                return core::Result<void>::failure(validation(
                    "parameter sweep axis values are not strictly increasing"));
            }
        }
    }

    std::size_t expected_case_count = 1U;
    for (const auto& axis : axes) {
        if (expected_case_count > kMaxParameterSweepCases / axis.values.size()) {
            return core::Result<void>::failure(validation(
                "parameter sweep case count exceeds the safety limit"));
        }
        expected_case_count *= axis.values.size();
    }
    if (cases.size() != expected_case_count) {
        return core::Result<void>::failure(validation(
            "parameter sweep case count does not match its axes"));
    }

    std::vector<std::vector<ParameterAssignment>> expected_cases;
    expected_cases.push_back(base_assignments);
    for (const auto& axis : axes) {
        std::vector<std::vector<ParameterAssignment>> expanded;
        expanded.reserve(expected_cases.size() * axis.values.size());
        for (const auto& current : expected_cases) {
            for (const double value : axis.values) {
                auto next = current;
                const auto assignment = std::find_if(
                    next.begin(), next.end(), [&axis](const ParameterAssignment& candidate) {
                        return candidate.parameter == axis.parameter;
                    });
                if (assignment == next.end()) {
                    return core::Result<void>::failure(validation(
                        "parameter sweep expansion lost an axis parameter"));
                }
                assignment->value = value;
                expanded.push_back(std::move(next));
            }
        }
        expected_cases = std::move(expanded);
    }
    for (std::size_t case_index = 0U; case_index < cases.size(); ++case_index) {
        if (cases[case_index].size() != base_assignments.size()) {
            return core::Result<void>::failure(validation(
                "parameter sweep case assignment count is invalid"));
        }
        for (std::size_t assignment_index = 0U;
             assignment_index < cases[case_index].size(); ++assignment_index) {
            if (auto result = cases[case_index][assignment_index].validate(); !result) {
                return result;
            }
            const auto& actual = cases[case_index][assignment_index];
            const auto& expected = expected_cases[case_index][assignment_index];
            if (actual.parameter != expected.parameter || actual.value != expected.value) {
                return core::Result<void>::failure(validation(
                    "parameter sweep case does not match its canonical expansion"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> Field::validate() const {
    if (auto result = validate_id(id.value, "field id"); !result) return result;
    if (static_cast<unsigned>(kind) > static_cast<unsigned>(FieldKind::categorical)) {
        return core::Result<void>::failure(validation("field kind is invalid"));
    }
    if (static_cast<unsigned>(association) > static_cast<unsigned>(Association::sample_set)) {
        return core::Result<void>::failure(validation("field association is invalid"));
    }
    if (auto result = validate_text(units, "field units", 128U); !result) return result;
    if (auto result = validate_text(coordinate_frame, "field coordinate frame", 256U); !result) return result;
    if (auto result = validate_text(time_or_stage, "field time/stage", 256U); !result) return result;
    if (auto result = validate_text(provider, "field provider", 256U); !result) return result;
    if (auto result = validate_digest(data_digest, "field data digest"); !result) return result;
    return provenance.validate();
}

core::Result<void> Probe::validate() const {
    if (auto result = validate_id(id.value, "probe id"); !result) return result;
    if (auto result = validate_text(name, "probe name", 256U, true); !result) return result;
    if (auto result = validate_text(location_reference, "probe location reference",
                                    kMaxReferenceBytes, true); !result) return result;
    return provenance.validate();
}

core::Result<void> Study::validate() const {
    if (auto result = validate_id(id.value, "study id"); !result) return result;
    if (source_model_revision.exhausted()) {
        return core::Result<void>::failure(validation("study source revision is exhausted"));
    }
    if (auto result = validate_id_list(regions, "study regions"); !result) return result;
    if (auto result = validate_id_list(boundaries, "study boundaries"); !result) return result;
    if (auto result = validate_id_list(interfaces, "study interfaces"); !result) return result;
    if (auto result = validate_id_list(sources, "study sources"); !result) return result;
    if (auto result = validate_id_list(loads, "study loads"); !result) return result;
    if (auto result = validate_id_list(parameters, "study parameters"); !result) return result;
    if (auto result = validate_id_list(requested_outputs, "study requested outputs"); !result) return result;
    if (auto result = validate_text(solver_configuration, "study solver configuration", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_text(provider, "study provider", 256U); !result) return result;
    if (auto result = validate_text(acceptance_metadata, "study acceptance metadata", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_digest(study_digest, "study digest"); !result) return result;
    return provenance.validate();
}

core::Result<void> ResultSet::validate() const {
    if (auto result = validate_id(id.value, "result set id"); !result) return result;
    if (auto result = validate_id(source_study.value, "result set source study"); !result) return result;
    if (auto result = validate_id_list(fields, "result set fields"); !result) return result;
    if (auto result = validate_id_list(probes, "result set probes"); !result) return result;
    if (auto result = validate_text(provider, "result set provider", 256U); !result) return result;
    if (auto result = validate_text(solver_version, "result set solver version", 256U); !result) return result;
    if (auto result = validate_text(convergence_evidence, "result set convergence evidence", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_digest(source_study_digest, "result set source study digest"); !result) return result;
    if (auto result = validate_digest(result_digest, "result set result digest"); !result) return result;
    return provenance.validate();
}

core::Result<void> MorphologyStructure::validate() const {
    if (auto result = validate_id(id.value, "morphology structure id"); !result) return result;
    if (auto result = validate_text(semantic_class, "morphology semantic class", 256U, true); !result) return result;
    if (parent_structure.has_value() && *parent_structure == id) {
        return core::Result<void>::failure(validation("morphology structure cannot parent itself"));
    }
    if (auto result = validate_text(laterality, "morphology laterality", 128U); !result) return result;
    if (auto result = validate_text(developmental_origin, "morphology developmental origin", kMaxReferenceBytes); !result) return result;
    if (auto result = geometry.validate(); !result) return result;
    if (auto result = validate_text(local_frame, "morphology local frame", 256U); !result) return result;
    if (auto result = validate_id_list(landmarks, "morphology structure landmarks"); !result) return result;
    if (auto result = validate_text(confidence, "morphology confidence", 128U); !result) return result;
    return provenance.validate();
}

core::Result<void> MorphologyAttachment::validate() const {
    if (auto result = validate_id(id.value, "morphology attachment id"); !result) return result;
    if (auto result = validate_id(first_structure.value, "morphology attachment first structure"); !result) return result;
    if (auto result = validate_id(second_structure.value, "morphology attachment second structure"); !result) return result;
    if (first_structure == second_structure) {
        return core::Result<void>::failure(validation("morphology attachment structures must be distinct"));
    }
    if (auto result = validate_text(relationship, "morphology attachment relationship", 128U, true); !result) return result;
    return provenance.validate();
}

core::Result<void> MorphologyLandmark::validate() const {
    if (auto result = validate_id(id.value, "morphology landmark id"); !result) return result;
    if (auto result = validate_text(name, "morphology landmark name", 256U, true); !result) return result;
    if (!position.finite()) {
        return core::Result<void>::failure(validation("morphology landmark position is not finite"));
    }
    if (auto result = validate_text(coordinate_frame, "morphology landmark coordinate frame", 256U, true); !result) return result;
    return provenance.validate();
}

core::Result<void> MorphologyGrowthDomain::validate() const {
    if (auto result = validate_id(id.value, "morphology growth domain id"); !result) return result;
    if (auto result = validate_id(structure.value, "morphology growth domain structure"); !result) {
        return result;
    }
    if (driving_field.has_value() && !driving_field->value) {
        return core::Result<void>::failure(validation(
            "morphology growth domain driving field must be non-zero"));
    }
    if (!growth_direction.finite() || growth_direction.length() <= 1e-12) {
        return core::Result<void>::failure(validation(
            "morphology growth domain direction must be finite and non-zero"));
    }
    if (!std::isfinite(growth_rate) || growth_rate < 0.0) {
        return core::Result<void>::failure(validation(
            "morphology growth domain rate must be finite and non-negative"));
    }
    if (!anisotropy.finite() || anisotropy.x < 0.0 || anisotropy.y < 0.0 ||
        anisotropy.z < 0.0 ||
        (anisotropy.x == 0.0 && anisotropy.y == 0.0 && anisotropy.z == 0.0)) {
        return core::Result<void>::failure(validation(
            "morphology growth domain anisotropy must be finite and non-zero"));
    }
    if (maximum_extent.has_value() &&
        (!std::isfinite(*maximum_extent) || *maximum_extent < 0.0)) {
        return core::Result<void>::failure(validation(
            "morphology growth domain maximum extent is invalid"));
    }
    if (auto result = validate_text(start_stage, "morphology growth domain start stage",
                                    256U, true); !result) {
        return result;
    }
    if (auto result = validate_text(end_stage, "morphology growth domain end stage",
                                    256U, true); !result) {
        return result;
    }
    return provenance.validate();
}

core::Result<void> MorphologyMeasurementPreview::validate() const {
    if (auto result = validate_id(structure.value, "morphology measurement structure"); !result) {
        return result;
    }
    if (auto result = validate_text(metric, "morphology measurement metric", 64U, true); !result) {
        return result;
    }
    if (auto result = validate_text(units, "morphology measurement units", 64U, true); !result) {
        return result;
    }
    if (auto result = validate_text(coordinate_frame,
                                    "morphology measurement coordinate frame", 256U, true);
        !result) {
        return result;
    }
    if (landmarks.empty() || landmarks.size() > kMaxCollectionEntries ||
        values.empty() || values.size() > 3U) {
        return core::Result<void>::failure(validation(
            "morphology measurement has an invalid bounded collection"));
    }
    std::set<LandmarkId> unique_landmarks;
    for (const auto landmark : landmarks) {
        if (auto result = validate_id(landmark.value, "morphology measurement landmark"); !result) {
            return result;
        }
        if (!unique_landmarks.insert(landmark).second) {
            return core::Result<void>::failure(validation(
                "morphology measurement contains a duplicate landmark"));
        }
    }
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<void>::failure(validation(
                "morphology measurement contains a non-finite value"));
        }
    }

    if (metric == "distance" || metric == "segment-length") {
        if (landmarks.size() != 2U || values.size() != 1U || units != "model" ||
            values.front() < 0.0) {
            return core::Result<void>::failure(validation(
                "morphology distance measurement has an invalid shape"));
        }
    } else if (metric == "arc-length") {
        if (landmarks.size() < 2U || values.size() != 1U || units != "model" ||
            values.front() < 0.0) {
            return core::Result<void>::failure(validation(
                "morphology arc-length measurement has an invalid shape"));
        }
    } else if (metric == "angle" || metric == "branch-angle") {
        if (landmarks.size() != 3U || values.size() != 1U || units != "radians" ||
            values.front() < 0.0 || values.front() > std::numbers::pi) {
            return core::Result<void>::failure(validation(
                "morphology angle measurement has an invalid shape"));
        }
    } else if (metric == "ratio") {
        if (landmarks.size() != 4U || values.size() != 1U || units != "ratio" ||
            values.front() < 0.0) {
            return core::Result<void>::failure(validation(
                "morphology ratio measurement has an invalid shape"));
        }
    } else if (metric == "area") {
        if (landmarks.size() < 3U || values.size() != 1U || units != "model^2" ||
            values.front() <= 0.0) {
            return core::Result<void>::failure(validation(
                "morphology area measurement has an invalid shape"));
        }
    } else if (metric == "volume") {
        if (landmarks.size() != 4U || values.size() != 1U || units != "model^3" ||
            values.front() <= 0.0) {
            return core::Result<void>::failure(validation(
                "morphology volume measurement has an invalid shape"));
        }
    } else if (metric == "centroid") {
        if (values.size() != 3U || units != "model") {
            return core::Result<void>::failure(validation(
                "morphology centroid measurement has an invalid shape"));
        }
    } else if (metric == "bounding-dimensions") {
        if (values.size() != 3U || units != "model" ||
            std::any_of(values.begin(), values.end(), [](double value) { return value < 0.0; })) {
            return core::Result<void>::failure(validation(
                "morphology bounding-dimensions measurement has an invalid shape"));
        }
    } else {
        return core::Result<void>::failure(validation(
            "morphology measurement metric is unsupported"));
    }
    return core::Result<void>::success();
}

core::Result<void> MorphologyLandmarkCorrespondence::validate() const {
    if (auto result = validate_id(reference_landmark.value,
                                  "morphology landmark correspondence reference landmark");
        !result) {
        return result;
    }
    if (auto result = validate_id(candidate_landmark.value,
                                  "morphology landmark correspondence candidate landmark");
        !result) {
        return result;
    }
    if (reference_landmark == candidate_landmark) {
        return core::Result<void>::failure(validation(
            "morphology landmark correspondence endpoints must be distinct"));
    }
    return core::Result<void>::success();
}

core::Result<void> MorphologySurfaceSample::validate() const {
    if (auto result = validate_id(reference_landmark.value,
                                  "morphology surface sample reference landmark"); !result) {
        return result;
    }
    if (auto result = validate_id(candidate_landmark.value,
                                  "morphology surface sample candidate landmark"); !result) {
        return result;
    }
    if (reference_landmark == candidate_landmark) {
        return core::Result<void>::failure(validation(
            "morphology surface sample endpoints must be distinct"));
    }
    const auto valid_normal = [](const core::Vec3d& normal) {
        return normal.finite() && std::isfinite(normal.length()) &&
            normal.length() > 1.0e-12;
    };
    if (!valid_normal(reference_normal) || !valid_normal(candidate_normal)) {
        return core::Result<void>::failure(validation(
            "morphology surface sample normals must be finite and non-zero"));
    }
    if (!std::isfinite(reference_curvature) || !std::isfinite(candidate_curvature) ||
        !std::isfinite(reference_thickness) || !std::isfinite(candidate_thickness) ||
        reference_thickness < 0.0 || candidate_thickness < 0.0) {
        return core::Result<void>::failure(validation(
            "morphology surface sample metadata is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> MorphologySurfaceDelta::validate() const {
    if (auto result = validate_id(reference_landmark.value,
                                  "morphology surface delta reference landmark"); !result) {
        return result;
    }
    if (auto result = validate_id(candidate_landmark.value,
                                  "morphology surface delta candidate landmark"); !result) {
        return result;
    }
    if (reference_landmark == candidate_landmark) {
        return core::Result<void>::failure(validation(
            "morphology surface delta endpoints must be distinct"));
    }
    if (!displacement.finite() || !std::isfinite(distance) || distance < 0.0 ||
        !std::isfinite(normal_displacement) || !std::isfinite(curvature_delta) ||
        !std::isfinite(thickness_delta) || !std::isfinite(normal_angle) ||
        normal_angle < 0.0 || normal_angle > std::numbers::pi) {
        return core::Result<void>::failure(validation(
            "morphology surface delta contains invalid values"));
    }
    const double measured = displacement.length();
    const double tolerance = 1.0e-12 * std::max(1.0, measured);
    if (!std::isfinite(measured) || std::abs(measured - distance) > tolerance) {
        return core::Result<void>::failure(validation(
            "morphology surface delta distance disagrees with displacement"));
    }
    return core::Result<void>::success();
}

core::Result<void> MorphologySurfaceDeltaPreview::validate() const {
    if (auto result = validate_id(reference_structure.value,
                                  "morphology surface delta preview reference structure"); !result) {
        return result;
    }
    if (auto result = validate_id(candidate_structure.value,
                                  "morphology surface delta preview candidate structure"); !result) {
        return result;
    }
    if (reference_structure == candidate_structure) {
        return core::Result<void>::failure(validation(
            "morphology surface delta preview structures must be distinct"));
    }
    if (auto result = validate_text(coordinate_frame,
                                    "morphology surface delta preview coordinate frame",
                                    256U, true); !result) {
        return result;
    }
    if (deltas.empty() || deltas.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "morphology surface delta preview must contain bounded samples"));
    }
    std::set<LandmarkId> reference_ids;
    std::set<LandmarkId> candidate_ids;
    double squared_sum = 0.0;
    for (const auto& delta : deltas) {
        if (auto result = delta.validate(); !result) return result;
        if (!reference_ids.insert(delta.reference_landmark).second ||
            !candidate_ids.insert(delta.candidate_landmark).second) {
            return core::Result<void>::failure(validation(
                "morphology surface delta preview contains duplicate correspondence"));
        }
        squared_sum += delta.distance * delta.distance;
        if (!std::isfinite(squared_sum)) {
            return core::Result<void>::failure(validation(
                "morphology surface delta preview RMS is not finite"));
        }
    }
    if (!std::isfinite(rms_displacement) || rms_displacement < 0.0) {
        return core::Result<void>::failure(validation(
            "morphology surface delta preview RMS is invalid"));
    }
    const double expected = std::sqrt(squared_sum / static_cast<double>(deltas.size()));
    const double tolerance = 1.0e-12 * std::max(1.0, expected);
    if (!std::isfinite(expected) || std::abs(expected - rms_displacement) > tolerance) {
        return core::Result<void>::failure(validation(
            "morphology surface delta preview RMS disagrees with samples"));
    }
    return core::Result<void>::success();
}

core::Result<void> MorphologyPrincipalAxesPreview::validate() const {
    if (auto result = validate_id(structure.value, "morphology principal-axes structure");
        !result) {
        return result;
    }
    if (auto result = validate_text(coordinate_frame,
                                    "morphology principal-axes coordinate frame",
                                    256U, true);
        !result) {
        return result;
    }
    if (landmarks.size() < 3U || landmarks.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "morphology principal-axes preview has an invalid landmark count"));
    }
    std::set<LandmarkId> unique_landmarks;
    for (const auto landmark : landmarks) {
        if (auto result = validate_id(landmark.value,
                                     "morphology principal-axes landmark"); !result) {
            return result;
        }
        if (!unique_landmarks.insert(landmark).second) {
            return core::Result<void>::failure(validation(
                "morphology principal-axes preview contains duplicate landmarks"));
        }
    }
    if (!centroid.finite()) {
        return core::Result<void>::failure(validation(
            "morphology principal-axes centroid is not finite"));
    }
    for (std::size_t index = 0U; index < axes.size(); ++index) {
        if (!axes[index].finite() ||
            std::abs(axes[index].length() - 1.0) > 1.0e-10 ||
            !std::isfinite(variances[index]) || variances[index] < 0.0) {
            return core::Result<void>::failure(validation(
                "morphology principal-axes preview contains an invalid axis"));
        }
        if (index > 0U && variances[index - 1U] < variances[index]) {
            return core::Result<void>::failure(validation(
                "morphology principal-axes variances are not ordered"));
        }
    }
    const double spectral_tolerance = 1.0e-10 *
        std::max(variances[0], std::numeric_limits<double>::min());
    if (variances[0] - variances[1] <= spectral_tolerance ||
        variances[1] - variances[2] <= spectral_tolerance) {
        return core::Result<void>::failure(validation(
            "morphology principal-axes spectrum is ambiguous"));
    }
    for (std::size_t left = 0U; left < axes.size(); ++left) {
        for (std::size_t right = left + 1U; right < axes.size(); ++right) {
            if (std::abs(core::dot(axes[left], axes[right])) > 1.0e-10) {
                return core::Result<void>::failure(validation(
                    "morphology principal-axes are not orthogonal"));
            }
        }
    }
    const double handedness = core::dot(core::cross(axes[0], axes[1]), axes[2]);
    if (!std::isfinite(handedness) || handedness < 1.0 - 1.0e-10) {
        return core::Result<void>::failure(validation(
            "morphology principal-axes are not right-handed"));
    }
    return core::Result<void>::success();
}

core::Result<void> BilateralMorphologyPreview::validate() const {
    if (auto result = validate_id(reference_structure.value,
                                  "bilateral morphology preview reference structure"); !result) {
        return result;
    }
    if (auto result = validate_id(candidate_structure.value,
                                  "bilateral morphology preview candidate structure"); !result) {
        return result;
    }
    if (reference_structure == candidate_structure) {
        return core::Result<void>::failure(validation(
            "bilateral morphology preview structures must be distinct"));
    }
    if (auto result = validate_text(alignment_mode, "bilateral morphology preview alignment mode",
                                    64U, true); !result) {
        return result;
    }
    if (alignment_mode != "landmark" && alignment_mode != "rigid" &&
        alignment_mode != "scale-normalized" && alignment_mode != "principal-axis") {
        return core::Result<void>::failure(validation(
            "bilateral morphology preview has an unsupported alignment mode"));
    }
    const double rotation_magnitude = std::hypot(
        std::hypot(alignment_rotation.x, alignment_rotation.y),
        std::hypot(alignment_rotation.z, alignment_rotation.w));
    if (!alignment_translation.finite() || !alignment_rotation.normalizable() ||
        !std::isfinite(rotation_magnitude) || std::abs(rotation_magnitude - 1.0) > 1.0e-12 ||
        !std::isfinite(alignment_scale) || alignment_scale <= 0.0) {
        return core::Result<void>::failure(validation(
            "bilateral morphology preview alignment transform is invalid"));
    }
    if (alignment_mode == "landmark" &&
        (alignment_translation.x != 0.0 || alignment_translation.y != 0.0 ||
         alignment_translation.z != 0.0 || alignment_rotation.x != 0.0 ||
         alignment_rotation.y != 0.0 || alignment_rotation.z != 0.0 ||
         alignment_rotation.w != 1.0 || alignment_scale != 1.0)) {
        return core::Result<void>::failure(validation(
            "shared-frame landmark preview must use the identity alignment transform"));
    }
    if (landmark_deltas.empty() || landmark_deltas.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "bilateral morphology preview must contain a bounded landmark correspondence"));
    }
    std::set<LandmarkId> reference_ids;
    std::set<LandmarkId> candidate_ids;
    for (const auto& delta : landmark_deltas) {
        if (auto result = validate_id(delta.reference_landmark.value,
                                      "bilateral morphology preview reference landmark"); !result) {
            return result;
        }
        if (auto result = validate_id(delta.candidate_landmark.value,
                                      "bilateral morphology preview candidate landmark"); !result) {
            return result;
        }
        if (!reference_ids.insert(delta.reference_landmark).second ||
            !candidate_ids.insert(delta.candidate_landmark).second) {
            return core::Result<void>::failure(validation(
                "bilateral morphology preview contains duplicate landmark correspondence"));
        }
        if (!delta.displacement.finite() || !std::isfinite(delta.distance) || delta.distance < 0.0) {
            return core::Result<void>::failure(validation(
                "bilateral morphology preview contains a non-finite landmark delta"));
        }
        const double measured = delta.displacement.length();
        const double tolerance = 1e-12 * std::max(1.0, measured);
        if (!std::isfinite(measured) || std::abs(measured - delta.distance) > tolerance) {
            return core::Result<void>::failure(validation(
                "bilateral morphology preview landmark distance disagrees with displacement"));
        }
    }
    if (!std::isfinite(rms_distance) || rms_distance < 0.0) {
        return core::Result<void>::failure(validation(
            "bilateral morphology preview RMS distance is invalid"));
    }
    double squared_sum = 0.0;
    for (const auto& delta : landmark_deltas) {
        squared_sum += delta.distance * delta.distance;
        if (!std::isfinite(squared_sum)) {
            return core::Result<void>::failure(validation(
                "bilateral morphology preview RMS distance is not finite"));
        }
    }
    const double expected_rms = std::sqrt(
        squared_sum / static_cast<double>(landmark_deltas.size()));
    const double rms_tolerance = 1.0e-12 * std::max(1.0, expected_rms);
    if (!std::isfinite(expected_rms) || std::abs(expected_rms - rms_distance) > rms_tolerance) {
        return core::Result<void>::failure(validation(
            "bilateral morphology preview RMS distance disagrees with landmark distances"));
    }
    return core::Result<void>::success();
}

core::Result<void> MorphologyModel::insert_structure(MorphologyStructure value) {
    return insert_unique(structures_, std::move(value), "morphology structure");
}

core::Result<void> MorphologyModel::insert_attachment(MorphologyAttachment value) {
    return insert_unique(attachments_, std::move(value), "morphology attachment");
}

core::Result<void> MorphologyModel::insert_landmark(MorphologyLandmark value) {
    return insert_unique(landmarks_, std::move(value), "morphology landmark");
}

core::Result<void> MorphologyModel::attach_landmark_to_structure(
    LandmarkId landmark,
    StructureId structure) {
    if (auto result = validate_id(landmark.value, "morphology landmark attachment landmark");
        !result) {
        return result;
    }
    if (auto result = validate_id(structure.value, "morphology landmark attachment structure");
        !result) {
        return result;
    }
    if (!landmarks_.contains(landmark)) {
        return core::Result<void>::failure(invalid(
            "morphology landmark attachment references an unknown landmark"));
    }
    const auto found = structures_.find(structure);
    if (found == structures_.end()) {
        return core::Result<void>::failure(invalid(
            "morphology landmark attachment references an unknown structure"));
    }
    auto updated = found->second;
    if (std::find(updated.landmarks.begin(), updated.landmarks.end(), landmark) !=
        updated.landmarks.end()) {
        return core::Result<void>::failure(invalid(
            "morphology landmark attachment is duplicated"));
    }
    updated.landmarks.push_back(landmark);
    if (auto result = updated.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "morphology landmark attachment"));
    }
    found->second = std::move(updated);
    return core::Result<void>::success();
}

core::Result<void> MorphologyModel::insert_growth_domain(MorphologyGrowthDomain value) {
    return insert_unique(growth_domains_, std::move(value), "morphology growth domain");
}

core::Result<MorphologyMeasurementPreview> MorphologyModel::preview_measurement(
    StructureId structure,
    std::string_view metric,
    std::span<const LandmarkId> landmarks) const {
    if (auto result = validate(); !result) {
        return core::Result<MorphologyMeasurementPreview>::failure(
            result.error().with_context("morphology measurement preview"));
    }
    if (auto result = validate_reference(structures_, structure,
                                         "morphology measurement structure"); !result) {
        return core::Result<MorphologyMeasurementPreview>::failure(result.error());
    }
    if (auto result = validate_text(metric, "morphology measurement metric", 64U, true); !result) {
        return core::Result<MorphologyMeasurementPreview>::failure(result.error());
    }
    if (metric != "distance" && metric != "segment-length" && metric != "arc-length" &&
        metric != "angle" && metric != "branch-angle" && metric != "ratio" &&
        metric != "area" && metric != "volume" && metric != "centroid" &&
        metric != "bounding-dimensions") {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology measurement metric is unsupported"));
    }
    if (landmarks.empty() || landmarks.size() > kMaxCollectionEntries) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology measurement requires a bounded non-empty landmark set"));
    }
    if ((metric == "distance" || metric == "segment-length") && landmarks.size() != 2U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology distance measurement requires two landmarks"));
    }
    if (metric == "angle" && landmarks.size() != 3U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology angle measurement requires three landmarks"));
    }
    if (metric == "branch-angle" && landmarks.size() != 3U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology branch-angle measurement requires three landmarks"));
    }
    if (metric == "ratio" && landmarks.size() != 4U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology ratio measurement requires four landmarks"));
    }
    if (metric == "arc-length" && landmarks.size() < 2U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology arc-length measurement requires at least two ordered landmarks"));
    }
    if (metric == "area" && landmarks.size() < 3U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology area measurement requires at least three ordered landmarks"));
    }
    if (metric == "volume" && landmarks.size() != 4U) {
        return core::Result<MorphologyMeasurementPreview>::failure(validation(
            "morphology volume measurement requires four tetrahedron landmarks"));
    }

    std::vector<core::Vec3d> positions;
    positions.reserve(landmarks.size());
    std::string coordinate_frame;
    std::set<LandmarkId> unique_landmarks;
    for (const auto landmark_id : landmarks) {
        if (auto result = validate_id(landmark_id.value, "morphology measurement landmark");
            !result) {
            return core::Result<MorphologyMeasurementPreview>::failure(result.error());
        }
        if (!unique_landmarks.insert(landmark_id).second) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology measurement contains a duplicate landmark"));
        }
        const auto landmark_iterator = landmarks_.find(landmark_id);
        if (landmark_iterator == landmarks_.end()) {
            return core::Result<MorphologyMeasurementPreview>::failure(invalid(
                "morphology measurement references an unknown landmark"));
        }
        const auto& landmark = landmark_iterator->second;
        if (!landmark.structure.has_value() || *landmark.structure != structure) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology measurement requires explicit landmark ownership"));
        }
        const auto& structure_record = structures_.at(structure);
        if (std::find(structure_record.landmarks.begin(), structure_record.landmarks.end(),
                      landmark_id) == structure_record.landmarks.end()) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology measurement landmark is not listed by its structure"));
        }
        if (coordinate_frame.empty()) {
            coordinate_frame = landmark.coordinate_frame;
        } else if (coordinate_frame != landmark.coordinate_frame) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology measurement requires a shared coordinate frame"));
        }
        positions.push_back(landmark.position);
    }

    MorphologyMeasurementPreview preview;
    preview.structure = structure;
    preview.metric = std::string(metric);
    preview.units = metric == "angle" || metric == "branch-angle" ? "radians" :
                    metric == "ratio" ? "ratio" :
                    metric == "area" ? "model^2" :
                    metric == "volume" ? "model^3" : "model";
    preview.coordinate_frame = std::move(coordinate_frame);
    preview.landmarks.assign(landmarks.begin(), landmarks.end());
    if (metric == "distance" || metric == "segment-length") {
        const double distance = (positions[1U] - positions[0U]).length();
        if (!std::isfinite(distance)) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology distance measurement is not finite"));
        }
        preview.values.push_back(distance);
    } else if (metric == "arc-length") {
        double length = 0.0;
        for (std::size_t index = 1U; index < positions.size(); ++index) {
            const double segment = (positions[index] - positions[index - 1U]).length();
            if (!std::isfinite(segment) || !std::isfinite(length + segment)) {
                return core::Result<MorphologyMeasurementPreview>::failure(validation(
                    "morphology arc-length measurement is not finite"));
            }
            length += segment;
        }
        preview.values.push_back(length);
    } else if (metric == "angle" || metric == "branch-angle") {
        const auto first = (positions[0U] - positions[1U]).normalized();
        const auto second = (positions[2U] - positions[1U]).normalized();
        if (!first.finite() || !second.finite()) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology angle measurement has a degenerate arm"));
        }
        const double cosine = std::clamp(core::dot(first, second), -1.0, 1.0);
        const double angle = std::acos(cosine);
        if (!std::isfinite(angle)) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology angle measurement is not finite"));
        }
        preview.values.push_back(angle);
    } else if (metric == "ratio") {
        const double numerator = (positions[1U] - positions[0U]).length();
        const double denominator = (positions[3U] - positions[2U]).length();
        if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator <= 0.0) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology ratio measurement has an invalid segment"));
        }
        const double ratio = numerator / denominator;
        if (!std::isfinite(ratio) || ratio < 0.0) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology ratio measurement is not finite"));
        }
        preview.values.push_back(ratio);
    } else if (metric == "area") {
        double scale = 1.0;
        for (const auto position : positions) {
            const double distance = (position - positions.front()).length();
            if (!std::isfinite(distance)) {
                return core::Result<MorphologyMeasurementPreview>::failure(validation(
                    "morphology area measurement has a non-finite extent"));
            }
            scale = std::max(scale, distance);
        }
        core::Vec3d normal{};
        bool found_normal = false;
        for (std::size_t first = 1U; first < positions.size() && !found_normal; ++first) {
            for (std::size_t second = first + 1U;
                 second < positions.size(); ++second) {
                const auto candidate = core::cross(
                    positions[first] - positions.front(),
                    positions[second] - positions.front());
                const double candidate_length = candidate.length();
                if (std::isfinite(candidate_length) &&
                    candidate_length > 1.0e-12 * scale * scale) {
                    normal = candidate * (1.0 / candidate_length);
                    found_normal = true;
                    break;
                }
            }
        }
        if (!found_normal || !normal.finite()) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology area measurement requires a non-degenerate polygon"));
        }
        const double planarity_tolerance = 1.0e-10 * scale;
        for (const auto position : positions) {
            const double distance_from_plane = core::dot(
                position - positions.front(), normal);
            if (!std::isfinite(distance_from_plane) ||
                std::abs(distance_from_plane) > planarity_tolerance) {
                return core::Result<MorphologyMeasurementPreview>::failure(validation(
                    "morphology area measurement requires a planar polygon"));
            }
        }
        const auto first_edge = positions[1U] - positions.front();
        const double first_edge_length = first_edge.length();
        if (!std::isfinite(first_edge_length) || first_edge_length <= 1.0e-12 * scale) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology area measurement contains a degenerate first edge"));
        }
        const auto basis_u = first_edge * (1.0 / first_edge_length);
        const auto basis_v = core::cross(normal, basis_u).normalized();
        if (!basis_u.finite() || !basis_v.finite()) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology area measurement has an invalid planar basis"));
        }
        struct Point2 {
            double x = 0.0;
            double y = 0.0;
        };
        std::vector<Point2> projected;
        projected.reserve(positions.size());
        for (const auto position : positions) {
            const auto offset = position - positions.front();
            projected.push_back({core::dot(offset, basis_u), core::dot(offset, basis_v)});
        }
        const auto orientation = [](const Point2& first_point,
                                    const Point2& second_point,
                                    const Point2& third_point) {
            return (second_point.x - first_point.x) * (third_point.y - first_point.y) -
                   (second_point.y - first_point.y) * (third_point.x - first_point.x);
        };
        const double intersection_tolerance = 1.0e-12 * scale * scale;
        const auto on_segment = [intersection_tolerance](
                                    const Point2& first_point,
                                    const Point2& second_point,
                                    const Point2& candidate) {
            return candidate.x >= std::min(first_point.x, second_point.x) - intersection_tolerance &&
                   candidate.x <= std::max(first_point.x, second_point.x) + intersection_tolerance &&
                   candidate.y >= std::min(first_point.y, second_point.y) - intersection_tolerance &&
                   candidate.y <= std::max(first_point.y, second_point.y) + intersection_tolerance;
        };
        const auto segments_intersect = [intersection_tolerance, &orientation, &on_segment](
                                            const Point2& first_start,
                                            const Point2& first_end,
                                            const Point2& second_start,
                                            const Point2& second_end) {
            const double first_orientation = orientation(first_start, first_end, second_start);
            const double second_orientation = orientation(first_start, first_end, second_end);
            const double third_orientation = orientation(second_start, second_end, first_start);
            const double fourth_orientation = orientation(second_start, second_end, first_end);
            const bool proper =
                ((first_orientation > intersection_tolerance && second_orientation < -intersection_tolerance) ||
                 (first_orientation < -intersection_tolerance && second_orientation > intersection_tolerance)) &&
                ((third_orientation > intersection_tolerance && fourth_orientation < -intersection_tolerance) ||
                 (third_orientation < -intersection_tolerance && fourth_orientation > intersection_tolerance));
            return proper ||
                   (std::abs(first_orientation) <= intersection_tolerance &&
                    on_segment(first_start, first_end, second_start)) ||
                   (std::abs(second_orientation) <= intersection_tolerance &&
                    on_segment(first_start, first_end, second_end)) ||
                   (std::abs(third_orientation) <= intersection_tolerance &&
                    on_segment(second_start, second_end, first_start)) ||
                   (std::abs(fourth_orientation) <= intersection_tolerance &&
                    on_segment(second_start, second_end, first_end));
        };
        for (std::size_t left = 0U; left < projected.size(); ++left) {
            const std::size_t left_next = (left + 1U) % projected.size();
            for (std::size_t right = left + 1U; right < projected.size(); ++right) {
                const std::size_t right_next = (right + 1U) % projected.size();
                if (left_next == right || right_next == left) {
                    continue;
                }
                if (segments_intersect(
                        projected[left], projected[left_next],
                        projected[right], projected[right_next])) {
                    return core::Result<MorphologyMeasurementPreview>::failure(validation(
                        "morphology area measurement requires a simple polygon"));
                }
            }
        }
        double area = 0.0;
        for (std::size_t index = 0U; index < positions.size(); ++index) {
            const auto& current = positions[index];
            const auto& next = positions[(index + 1U) % positions.size()];
            const double edge_length = (next - current).length();
            if (!std::isfinite(edge_length) || edge_length <= 1.0e-12 * scale) {
                return core::Result<MorphologyMeasurementPreview>::failure(validation(
                    "morphology area measurement contains a degenerate edge"));
            }
            area += core::dot(core::cross(
                current - positions.front(), next - positions.front()), normal);
            if (!std::isfinite(area)) {
                return core::Result<MorphologyMeasurementPreview>::failure(validation(
                    "morphology area measurement is not finite"));
            }
        }
        area = std::abs(area) * 0.5;
        if (!std::isfinite(area) || area <= 1.0e-12 * scale * scale) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology area measurement is degenerate"));
        }
        preview.values.push_back(area);
    } else if (metric == "volume") {
        const auto first = positions[1U] - positions[0U];
        const auto second = positions[2U] - positions[0U];
        const auto third = positions[3U] - positions[0U];
        const double volume = std::abs(core::dot(first, core::cross(second, third))) / 6.0;
        const double scale = std::max({1.0, first.length(), second.length(), third.length()});
        if (!std::isfinite(volume) || volume <= 1.0e-15 * scale * scale * scale) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology volume measurement requires a non-degenerate tetrahedron"));
        }
        preview.values.push_back(volume);
    } else if (metric == "centroid") {
        core::Vec3d sum{};
        for (const auto position : positions) {
            sum = sum + position;
            if (!sum.finite()) {
                return core::Result<MorphologyMeasurementPreview>::failure(validation(
                    "morphology centroid measurement is not finite"));
            }
        }
        const auto centroid = sum * (1.0 / static_cast<double>(positions.size()));
        if (!centroid.finite()) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology centroid measurement is not finite"));
        }
        preview.values = {centroid.x, centroid.y, centroid.z};
    } else {
        core::Vec3d minimum = positions.front();
        core::Vec3d maximum = positions.front();
        for (const auto position : positions) {
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
        preview.values = {
            maximum.x - minimum.x,
            maximum.y - minimum.y,
            maximum.z - minimum.z,
        };
        if (!std::all_of(preview.values.begin(), preview.values.end(),
                         [](double value) { return std::isfinite(value) && value >= 0.0; })) {
            return core::Result<MorphologyMeasurementPreview>::failure(validation(
                "morphology bounding-dimensions measurement is invalid"));
        }
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<MorphologyMeasurementPreview>::failure(result.error());
    }
    return core::Result<MorphologyMeasurementPreview>::success(std::move(preview));
}

core::Result<MorphologyPrincipalAxesPreview> MorphologyModel::preview_principal_axes(
    StructureId structure,
    std::span<const LandmarkId> landmarks) const {
    if (auto result = validate(); !result) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(
            result.error().with_context("morphology principal-axes preview"));
    }
    if (auto result = validate_reference(structures_, structure,
                                         "morphology principal-axes structure"); !result) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(result.error());
    }
    if (landmarks.size() < 3U || landmarks.size() > kMaxCollectionEntries) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
            "morphology principal-axes preview requires at least three bounded landmarks"));
    }

    const auto& structure_record = structures_.at(structure);
    std::vector<core::Vec3d> positions;
    positions.reserve(landmarks.size());
    std::string coordinate_frame;
    std::set<LandmarkId> unique_landmarks;
    for (const auto landmark_id : landmarks) {
        if (auto result = validate_id(landmark_id.value,
                                     "morphology principal-axes landmark"); !result) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(result.error());
        }
        if (!unique_landmarks.insert(landmark_id).second) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
                "morphology principal-axes preview contains duplicate landmarks"));
        }
        if (std::find(structure_record.landmarks.begin(), structure_record.landmarks.end(),
                      landmark_id) == structure_record.landmarks.end()) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
                "morphology principal-axes landmark is not listed by its structure"));
        }
        const auto landmark_iterator = landmarks_.find(landmark_id);
        if (landmark_iterator == landmarks_.end()) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(invalid(
                "morphology principal-axes preview references an unknown landmark"));
        }
        const auto& landmark = landmark_iterator->second;
        if (!landmark.structure.has_value() || *landmark.structure != structure) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
                "morphology principal-axes preview requires explicit landmark ownership"));
        }
        if (coordinate_frame.empty()) {
            coordinate_frame = landmark.coordinate_frame;
        } else if (coordinate_frame != landmark.coordinate_frame) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
                "morphology principal-axes preview requires a shared coordinate frame"));
        }
        positions.push_back(landmark.position);
    }

    core::Vec3d centroid{};
    for (const auto position : positions) {
        centroid = centroid + position;
        if (!centroid.finite()) {
            return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
                "morphology principal-axes centroid is not finite"));
        }
    }
    centroid = centroid * (1.0 / static_cast<double>(positions.size()));
    if (!centroid.finite()) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
            "morphology principal-axes centroid is not finite"));
    }

    const auto analysis = compute_principal_axes(positions);
    if (!analysis.has_value()) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
            "morphology principal-axes spread is degenerate or ambiguous"));
    }

    const auto canonical_axis = [](core::Vec3d axis) {
        const auto component_at = [&axis](std::size_t index) {
            return index == 0U ? axis.x : index == 1U ? axis.y : axis.z;
        };
        std::size_t dominant = 0U;
        if (std::abs(axis.y) > std::abs(axis.x)) dominant = 1U;
        if (std::abs(axis.z) > std::abs(component_at(dominant))) dominant = 2U;
        const double component = component_at(dominant);
        if (component < 0.0) axis = axis * -1.0;
        return axis;
    };
    const auto& raw_axes = analysis->axes;
    MorphologyPrincipalAxesPreview preview;
    preview.structure = structure;
    preview.coordinate_frame = std::move(coordinate_frame);
    preview.landmarks.assign(landmarks.begin(), landmarks.end());
    preview.centroid = centroid;
    preview.axes[0] = canonical_axis(raw_axes[0]);
    preview.axes[1] = canonical_axis(raw_axes[1]);
    preview.axes[2] = core::cross(preview.axes[0], preview.axes[1]).normalized();
    if (!preview.axes[2].finite() ||
        std::abs(core::dot(preview.axes[2], raw_axes[2])) < 1.0e-8) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(validation(
            "morphology principal-axes orientation is ambiguous"));
    }
    preview.variances = analysis->variances;
    if (auto result = preview.validate(); !result) {
        return core::Result<MorphologyPrincipalAxesPreview>::failure(result.error());
    }
    return core::Result<MorphologyPrincipalAxesPreview>::success(std::move(preview));
}

core::Result<MorphologySurfaceDeltaPreview> MorphologyModel::preview_surface_delta(
    StructureId reference_structure,
    StructureId candidate_structure,
    std::span<const MorphologySurfaceSample> samples) const {
    if (auto result = validate(); !result) {
        return core::Result<MorphologySurfaceDeltaPreview>::failure(
            result.error().with_context("morphology surface delta preview"));
    }
    if (auto result = validate_reference(
            structures_, reference_structure,
            "morphology surface delta preview reference structure"); !result) {
        return core::Result<MorphologySurfaceDeltaPreview>::failure(result.error());
    }
    if (auto result = validate_reference(
            structures_, candidate_structure,
            "morphology surface delta preview candidate structure"); !result) {
        return core::Result<MorphologySurfaceDeltaPreview>::failure(result.error());
    }
    if (reference_structure == candidate_structure) {
        return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
            "morphology surface delta preview structures must be distinct"));
    }
    if (samples.empty() || samples.size() > kMaxCollectionEntries) {
        return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
            "morphology surface delta preview requires bounded samples"));
    }

    const auto& reference = structures_.at(reference_structure);
    const auto& candidate = structures_.at(candidate_structure);
    std::set<LandmarkId> reference_ids;
    std::set<LandmarkId> candidate_ids;
    std::string coordinate_frame;
    MorphologySurfaceDeltaPreview preview;
    preview.reference_structure = reference_structure;
    preview.candidate_structure = candidate_structure;
    preview.deltas.reserve(samples.size());
    double squared_sum = 0.0;

    for (const auto& sample : samples) {
        if (auto result = sample.validate(); !result) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(result.error());
        }
        if (!reference_ids.insert(sample.reference_landmark).second ||
            !candidate_ids.insert(sample.candidate_landmark).second) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
                "morphology surface delta preview contains duplicate correspondence"));
        }
        const auto reference_landmark = landmarks_.find(sample.reference_landmark);
        const auto candidate_landmark = landmarks_.find(sample.candidate_landmark);
        if (reference_landmark == landmarks_.end() || candidate_landmark == landmarks_.end()) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(invalid(
                "morphology surface delta preview references an unknown landmark"));
        }
        if (!reference_landmark->second.structure.has_value() ||
            *reference_landmark->second.structure != reference_structure ||
            !candidate_landmark->second.structure.has_value() ||
            *candidate_landmark->second.structure != candidate_structure) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
                "morphology surface delta preview requires explicit landmark ownership"));
        }
        if (std::find(reference.landmarks.begin(), reference.landmarks.end(),
                      sample.reference_landmark) == reference.landmarks.end() ||
            std::find(candidate.landmarks.begin(), candidate.landmarks.end(),
                      sample.candidate_landmark) == candidate.landmarks.end()) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
                "morphology surface delta preview landmark is not listed by its structure"));
        }
        const auto& reference_record = reference_landmark->second;
        const auto& candidate_record = candidate_landmark->second;
        if (reference_record.coordinate_frame != candidate_record.coordinate_frame) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
                "morphology surface delta preview requires a shared coordinate frame"));
        }
        if (coordinate_frame.empty()) {
            coordinate_frame = reference_record.coordinate_frame;
        } else if (coordinate_frame != reference_record.coordinate_frame) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
                "morphology surface delta preview samples must share a coordinate frame"));
        }

        const auto displacement = candidate_record.position - reference_record.position;
        const auto reference_normal = sample.reference_normal.normalized();
        const auto candidate_normal = sample.candidate_normal.normalized();
        const double distance = displacement.length();
        const double normal_dot = std::clamp(
            core::dot(reference_normal, candidate_normal), -1.0, 1.0);
        const double normal_angle = std::acos(normal_dot);
        const double normal_displacement = core::dot(displacement, reference_normal);
        const MorphologySurfaceDelta delta{
            sample.reference_landmark,
            sample.candidate_landmark,
            displacement,
            distance,
            normal_displacement,
            sample.candidate_curvature - sample.reference_curvature,
            sample.candidate_thickness - sample.reference_thickness,
            normal_angle};
        if (auto result = delta.validate(); !result) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(result.error());
        }
        preview.deltas.push_back(delta);
        squared_sum += distance * distance;
        if (!std::isfinite(squared_sum)) {
            return core::Result<MorphologySurfaceDeltaPreview>::failure(validation(
                "morphology surface delta preview RMS is not finite"));
        }
    }
    preview.coordinate_frame = std::move(coordinate_frame);
    preview.rms_displacement = std::sqrt(
        squared_sum / static_cast<double>(preview.deltas.size()));
    if (auto result = preview.validate(); !result) {
        return core::Result<MorphologySurfaceDeltaPreview>::failure(result.error());
    }
    return core::Result<MorphologySurfaceDeltaPreview>::success(std::move(preview));
}

core::Result<BilateralMorphologyPreview> MorphologyModel::preview_bilateral_landmarks(
    StructureId reference_structure,
    StructureId candidate_structure) const {
    return preview_bilateral_landmarks(reference_structure, candidate_structure, "landmark");
}

core::Result<BilateralMorphologyPreview> MorphologyModel::preview_bilateral_landmarks(
    StructureId reference_structure,
    StructureId candidate_structure,
    std::string_view alignment_mode) const {
    return preview_bilateral_landmarks(
        reference_structure, candidate_structure,
        std::span<const MorphologyLandmarkCorrespondence>{}, alignment_mode);
}

core::Result<BilateralMorphologyPreview> MorphologyModel::preview_bilateral_landmarks(
    StructureId reference_structure,
    StructureId candidate_structure,
    std::span<const MorphologyLandmarkCorrespondence> requested_correspondences,
    std::string_view alignment_mode) const {
    if (auto result = validate(); !result) {
        return core::Result<BilateralMorphologyPreview>::failure(
            result.error().with_context("bilateral morphology preview"));
    }
    if (auto result = validate_reference(structures_, reference_structure,
                                         "bilateral morphology preview reference structure"); !result) {
        return core::Result<BilateralMorphologyPreview>::failure(result.error());
    }
    if (auto result = validate_reference(structures_, candidate_structure,
                                         "bilateral morphology preview candidate structure"); !result) {
        return core::Result<BilateralMorphologyPreview>::failure(result.error());
    }
    if (reference_structure == candidate_structure) {
        return core::Result<BilateralMorphologyPreview>::failure(validation(
            "bilateral morphology preview structures must be distinct"));
    }
    if (alignment_mode != "landmark" && alignment_mode != "rigid" &&
        alignment_mode != "scale-normalized" && alignment_mode != "principal-axis") {
        return core::Result<BilateralMorphologyPreview>::failure(validation(
            "bilateral morphology preview alignment mode is unsupported"));
    }
    const auto& reference = structures_.at(reference_structure);
    const auto& candidate = structures_.at(candidate_structure);
    if (reference.landmarks.empty() || candidate.landmarks.empty()) {
        return core::Result<BilateralMorphologyPreview>::failure(validation(
            "bilateral morphology preview requires non-empty landmark sets"));
    }

    struct Correspondence {
        LandmarkId reference_id;
        LandmarkId candidate_id;
        core::Vec3d reference_position;
        core::Vec3d candidate_position;
    };
    std::vector<Correspondence> correspondences;

    const auto append_correspondence = [&](LandmarkId reference_id,
                                            LandmarkId candidate_id)
        -> core::Result<void> {
        const auto reference_iterator = landmarks_.find(reference_id);
        const auto candidate_iterator = landmarks_.find(candidate_id);
        if (reference_iterator == landmarks_.end() || candidate_iterator == landmarks_.end()) {
            return core::Result<void>::failure(invalid(
                "bilateral morphology preview correspondence references an unknown landmark"));
        }
        const auto& reference_landmark = reference_iterator->second;
        const auto& candidate_landmark = candidate_iterator->second;
        if (!reference_landmark.structure.has_value() ||
            *reference_landmark.structure != reference_structure ||
            !candidate_landmark.structure.has_value() ||
            *candidate_landmark.structure != candidate_structure) {
            return core::Result<void>::failure(validation(
                "bilateral morphology preview correspondence has invalid structure ownership"));
        }
        if (reference_landmark.coordinate_frame != candidate_landmark.coordinate_frame) {
            return core::Result<void>::failure(validation(
                "bilateral morphology preview requires a shared landmark coordinate frame"));
        }
        correspondences.push_back(Correspondence{
            reference_id, candidate_id,
            reference_landmark.position, candidate_landmark.position});
        return core::Result<void>::success();
    };

    if (!requested_correspondences.empty()) {
        if (requested_correspondences.size() > kMaxCollectionEntries) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "bilateral morphology preview correspondence set exceeds the safety limit"));
        }
        std::set<LandmarkId> reference_ids;
        std::set<LandmarkId> candidate_ids;
        correspondences.reserve(requested_correspondences.size());
        for (const auto& requested : requested_correspondences) {
            if (auto result = requested.validate(); !result) {
                return core::Result<BilateralMorphologyPreview>::failure(result.error());
            }
            if (!reference_ids.insert(requested.reference_landmark).second ||
                !candidate_ids.insert(requested.candidate_landmark).second) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "bilateral morphology preview correspondence set contains duplicates"));
            }
            if (std::find(reference.landmarks.begin(), reference.landmarks.end(),
                          requested.reference_landmark) == reference.landmarks.end() ||
                std::find(candidate.landmarks.begin(), candidate.landmarks.end(),
                          requested.candidate_landmark) == candidate.landmarks.end()) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "bilateral morphology preview correspondence is not owned by its structure"));
            }
            if (auto result = append_correspondence(
                    requested.reference_landmark, requested.candidate_landmark); !result) {
                return core::Result<BilateralMorphologyPreview>::failure(result.error());
            }
        }
    } else {
        if (reference.landmarks.size() != candidate.landmarks.size()) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "bilateral morphology preview requires equal landmark sets without an explicit map"));
        }
        const auto named_landmarks = [&](const MorphologyStructure& structure,
                                         std::string_view side)
            -> core::Result<std::map<std::string, LandmarkId>> {
            std::map<std::string, LandmarkId> result;
            for (const auto landmark_id : structure.landmarks) {
                const auto& landmark = landmarks_.at(landmark_id);
                if (!landmark.structure.has_value() || *landmark.structure != structure.id) {
                    return core::Result<std::map<std::string, LandmarkId>>::failure(validation(
                        "bilateral morphology preview requires explicit landmark ownership on " +
                        std::string(side) + " structure"));
                }
                if (!result.emplace(landmark.name, landmark.id).second) {
                    return core::Result<std::map<std::string, LandmarkId>>::failure(validation(
                        "bilateral morphology preview requires unique landmark names per structure"));
                }
            }
            return core::Result<std::map<std::string, LandmarkId>>::success(std::move(result));
        };
        const auto reference_landmarks = named_landmarks(reference, "reference");
        if (!reference_landmarks) {
            return core::Result<BilateralMorphologyPreview>::failure(reference_landmarks.error());
        }
        const auto candidate_landmarks = named_landmarks(candidate, "candidate");
        if (!candidate_landmarks) {
            return core::Result<BilateralMorphologyPreview>::failure(candidate_landmarks.error());
        }
        if (reference_landmarks.value().size() != candidate_landmarks.value().size()) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "bilateral morphology preview landmark names do not correspond"));
        }
        correspondences.reserve(reference_landmarks.value().size());
        for (const auto& [name, reference_id] : reference_landmarks.value()) {
            const auto candidate_iterator = candidate_landmarks.value().find(name);
            if (candidate_iterator == candidate_landmarks.value().end()) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "bilateral morphology preview landmark names do not correspond"));
            }
            if (auto result = append_correspondence(reference_id, candidate_iterator->second);
                !result) {
                return core::Result<BilateralMorphologyPreview>::failure(result.error());
            }
        }
    }

    BilateralMorphologyPreview preview;
    preview.reference_structure = reference_structure;
    preview.candidate_structure = candidate_structure;
    preview.alignment_mode = std::string(alignment_mode);
    preview.landmark_deltas.reserve(correspondences.size());

    if (alignment_mode != "landmark") {
        if (correspondences.size() < 3U) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "aligned bilateral morphology preview requires at least three landmarks"));
        }

        core::Vec3d reference_centroid{};
        core::Vec3d candidate_centroid{};
        for (const auto& correspondence : correspondences) {
            reference_centroid = reference_centroid + correspondence.reference_position;
            candidate_centroid = candidate_centroid + correspondence.candidate_position;
        }
        const double count = static_cast<double>(correspondences.size());
        reference_centroid = reference_centroid * (1.0 / count);
        candidate_centroid = candidate_centroid * (1.0 / count);
        if (!reference_centroid.finite() || !candidate_centroid.finite()) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "aligned bilateral morphology preview centroid is not finite"));
        }

        struct Basis {
            core::Vec3d first;
            core::Vec3d second;
            core::Vec3d third;
        };
        const auto make_basis = [](core::Vec3d origin, core::Vec3d first,
                                   core::Vec3d second) -> std::optional<Basis> {
            const auto first_axis = (first - origin).normalized();
            if (!first_axis.finite()) return std::nullopt;
            const auto second_offset = second - origin;
            const auto orthogonal = second_offset -
                first_axis * core::dot(second_offset, first_axis);
            const auto second_axis = orthogonal.normalized();
            if (!second_axis.finite()) return std::nullopt;
            const auto third_axis = core::cross(first_axis, second_axis);
            if (!third_axis.finite() || third_axis.length() <= 1.0e-12) {
                return std::nullopt;
            }
            return Basis{first_axis, second_axis, third_axis.normalized()};
        };

        double reference_variance = 0.0;
        double candidate_variance = 0.0;
        for (const auto& correspondence : correspondences) {
            reference_variance +=
                (correspondence.reference_position - reference_centroid).length_squared();
            candidate_variance +=
                (correspondence.candidate_position - candidate_centroid).length_squared();
        }
        if (!std::isfinite(reference_variance) || !std::isfinite(candidate_variance) ||
            candidate_variance <= 1.0e-24) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "aligned bilateral morphology preview has invalid landmark spread"));
        }
        if (alignment_mode == "scale-normalized") {
            preview.alignment_scale = std::sqrt(reference_variance / candidate_variance);
            if (!std::isfinite(preview.alignment_scale) || preview.alignment_scale <= 0.0) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "scale-normalized morphology alignment scale is invalid"));
            }
        }

        const auto principal_axes = [&](bool reference_side)
            -> std::optional<std::array<core::Vec3d, 3>> {
            std::vector<core::Vec3d> positions;
            positions.reserve(correspondences.size());
            for (const auto& correspondence : correspondences) {
                positions.push_back(reference_side
                    ? correspondence.reference_position
                    : correspondence.candidate_position);
            }
            const auto analysis = compute_principal_axes(positions);
            if (!analysis.has_value()) return std::nullopt;
            return analysis->axes;
        };

        std::array<core::Vec3d, 3> reference_axes{};
        std::array<core::Vec3d, 3> candidate_axes{};
        if (alignment_mode == "principal-axis") {
            const auto reference_principal_axes = principal_axes(true);
            const auto candidate_principal_axes = principal_axes(false);
            if (!reference_principal_axes.has_value() ||
                !candidate_principal_axes.has_value()) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "principal-axis morphology alignment is underconstrained or ambiguous"));
            }
            reference_axes = reference_principal_axes.value();
            candidate_axes = candidate_principal_axes.value();
        } else {
            std::size_t first_distinct = correspondences.size();
            for (std::size_t index = 1U; index < correspondences.size(); ++index) {
                if ((correspondences[index].reference_position -
                     correspondences[0].reference_position).length() > 1.0e-12 &&
                    (correspondences[index].candidate_position -
                     correspondences[0].candidate_position).length() > 1.0e-12) {
                    first_distinct = index;
                    break;
                }
            }
            if (first_distinct == correspondences.size()) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "aligned bilateral morphology preview landmarks are coincident"));
            }

            std::size_t third_point = correspondences.size();
            for (std::size_t index = first_distinct + 1U; index < correspondences.size(); ++index) {
                const auto reference_cross = core::cross(
                    correspondences[first_distinct].reference_position -
                        correspondences[0].reference_position,
                    correspondences[index].reference_position -
                        correspondences[0].reference_position);
                const auto candidate_cross = core::cross(
                    correspondences[first_distinct].candidate_position -
                        correspondences[0].candidate_position,
                    correspondences[index].candidate_position -
                        correspondences[0].candidate_position);
                if (reference_cross.finite() && candidate_cross.finite() &&
                    reference_cross.length() > 1.0e-12 && candidate_cross.length() > 1.0e-12) {
                    third_point = index;
                    break;
                }
            }
            if (third_point == correspondences.size()) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "aligned bilateral morphology preview requires non-collinear landmarks"));
            }
            const auto reference_basis = make_basis(
                correspondences[0].reference_position,
                correspondences[first_distinct].reference_position,
                correspondences[third_point].reference_position);
            const auto candidate_basis = make_basis(
                correspondences[0].candidate_position,
                correspondences[first_distinct].candidate_position,
                correspondences[third_point].candidate_position);
            if (!reference_basis.has_value() || !candidate_basis.has_value()) {
                return core::Result<BilateralMorphologyPreview>::failure(validation(
                    "aligned bilateral morphology preview basis is degenerate"));
            }
            reference_axes = {reference_basis->first, reference_basis->second, reference_basis->third};
            candidate_axes = {candidate_basis->first, candidate_basis->second, candidate_basis->third};
        }

        const auto component = [](core::Vec3d value, std::size_t index) {
            return index == 0U ? value.x : index == 1U ? value.y : value.z;
        };
        const auto make_rotation = [&](const std::array<core::Vec3d, 3>& reference,
                                       const std::array<core::Vec3d, 3>& candidate)
            -> std::optional<core::Quaternion> {
            std::array<std::array<double, 3>, 3> rotation_matrix{};
            for (std::size_t row = 0U; row < 3U; ++row) {
                for (std::size_t column = 0U; column < 3U; ++column) {
                    for (std::size_t axis = 0U; axis < 3U; ++axis) {
                        rotation_matrix[row][column] +=
                            component(reference[axis], row) *
                            component(candidate[axis], column);
                    }
                    if (!std::isfinite(rotation_matrix[row][column])) return std::nullopt;
                }
            }
            const double trace = rotation_matrix[0][0] + rotation_matrix[1][1] +
                rotation_matrix[2][2];
            core::Quaternion rotation{};
            if (trace > 0.0) {
                const double root = std::sqrt(trace + 1.0) * 2.0;
                if (!std::isfinite(root) || root <= 0.0) return std::nullopt;
                rotation.w = 0.25 * root;
                rotation.x = (rotation_matrix[2][1] - rotation_matrix[1][2]) / root;
                rotation.y = (rotation_matrix[0][2] - rotation_matrix[2][0]) / root;
                rotation.z = (rotation_matrix[1][0] - rotation_matrix[0][1]) / root;
            } else if (rotation_matrix[0][0] > rotation_matrix[1][1] &&
                       rotation_matrix[0][0] > rotation_matrix[2][2]) {
                const double root = std::sqrt(std::max(
                    0.0, 1.0 + rotation_matrix[0][0] - rotation_matrix[1][1] -
                        rotation_matrix[2][2])) * 2.0;
                if (!std::isfinite(root) || root <= 0.0) return std::nullopt;
                rotation.w = (rotation_matrix[2][1] - rotation_matrix[1][2]) / root;
                rotation.x = 0.25 * root;
                rotation.y = (rotation_matrix[0][1] + rotation_matrix[1][0]) / root;
                rotation.z = (rotation_matrix[0][2] + rotation_matrix[2][0]) / root;
            } else if (rotation_matrix[1][1] > rotation_matrix[2][2]) {
                const double root = std::sqrt(std::max(
                    0.0, 1.0 + rotation_matrix[1][1] - rotation_matrix[0][0] -
                        rotation_matrix[2][2])) * 2.0;
                if (!std::isfinite(root) || root <= 0.0) return std::nullopt;
                rotation.w = (rotation_matrix[0][2] - rotation_matrix[2][0]) / root;
                rotation.x = (rotation_matrix[0][1] + rotation_matrix[1][0]) / root;
                rotation.y = 0.25 * root;
                rotation.z = (rotation_matrix[1][2] + rotation_matrix[2][1]) / root;
            } else {
                const double root = std::sqrt(std::max(
                    0.0, 1.0 + rotation_matrix[2][2] - rotation_matrix[0][0] -
                        rotation_matrix[1][1])) * 2.0;
                if (!std::isfinite(root) || root <= 0.0) return std::nullopt;
                rotation.w = (rotation_matrix[1][0] - rotation_matrix[0][1]) / root;
                rotation.x = (rotation_matrix[0][2] + rotation_matrix[2][0]) / root;
                rotation.y = (rotation_matrix[1][2] + rotation_matrix[2][1]) / root;
                rotation.z = 0.25 * root;
            }
            if (!rotation.normalizable()) return std::nullopt;
            return rotation.normalized();
        };

        std::optional<core::Quaternion> selected_rotation;
        core::Vec3d selected_translation{};
        double selected_error = std::numeric_limits<double>::infinity();
        for (const int first_sign : {-1, 1}) {
            for (const int second_sign : {-1, 1}) {
                const std::array<int, 3> signs{
                    first_sign, second_sign, first_sign * second_sign};
                std::array<core::Vec3d, 3> signed_candidate_axes{};
                for (std::size_t axis = 0U; axis < 3U; ++axis) {
                    signed_candidate_axes[axis] = candidate_axes[axis] *
                        static_cast<double>(signs[axis]);
                }
                const auto rotation = make_rotation(reference_axes, signed_candidate_axes);
                if (!rotation.has_value()) continue;
                const auto translation = reference_centroid -
                    rotation->rotate(candidate_centroid * preview.alignment_scale);
                if (!translation.finite()) continue;
                double error = 0.0;
                bool valid_error = true;
                for (const auto& correspondence : correspondences) {
                    const auto transformed = rotation->rotate(
                        correspondence.candidate_position * preview.alignment_scale) + translation;
                    const auto displacement = transformed - correspondence.reference_position;
                    const double distance = displacement.length();
                    if (!std::isfinite(distance) ||
                        !std::isfinite(error + distance * distance)) {
                        valid_error = false;
                        break;
                    }
                    error += distance * distance;
                }
                if (valid_error && error < selected_error) {
                    selected_rotation = rotation;
                    selected_translation = translation;
                    selected_error = error;
                }
            }
        }
        if (!selected_rotation.has_value()) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "aligned bilateral morphology preview rotation is invalid"));
        }
        preview.alignment_rotation = selected_rotation.value();
        preview.alignment_translation = selected_translation;
    }

    double squared_sum = 0.0;
    for (const auto& correspondence : correspondences) {
        core::Vec3d transformed_candidate = correspondence.candidate_position;
        if (alignment_mode != "landmark") {
            transformed_candidate = preview.alignment_rotation.rotate(
                correspondence.candidate_position * preview.alignment_scale) +
                preview.alignment_translation;
        }
        if (!transformed_candidate.finite()) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "bilateral morphology preview produced a non-finite aligned position"));
        }
        const auto displacement = transformed_candidate - correspondence.reference_position;
        const double distance = displacement.length();
        if (!std::isfinite(distance) || !std::isfinite(squared_sum + distance * distance)) {
            return core::Result<BilateralMorphologyPreview>::failure(validation(
                "bilateral morphology preview produced a non-finite landmark distance"));
        }
        squared_sum += distance * distance;
        preview.landmark_deltas.push_back(MorphologyLandmarkDelta{
            correspondence.reference_id, correspondence.candidate_id, displacement, distance});
    }
    preview.rms_distance = std::sqrt(squared_sum /
                                     static_cast<double>(preview.landmark_deltas.size()));
    if (auto result = preview.validate(); !result) {
        return core::Result<BilateralMorphologyPreview>::failure(result.error());
    }
    return core::Result<BilateralMorphologyPreview>::success(std::move(preview));
}

core::Result<void> MorphologyModel::validate() const {
    if (auto result = validate_text(organism_identity, "morphology organism identity", 256U, true); !result) return result;
    if (auto result = validate_text(body_plan, "morphology body plan", 256U, true); !result) return result;
    if (auto result = validate_text(symmetry, "morphology symmetry", 128U); !result) return result;
    if (axes.size() > 32U) return core::Result<void>::failure(validation("morphology has too many axes"));
    std::set<std::string> unique_axes;
    for (const auto& axis : axes) {
        if (auto result = validate_text(axis, "morphology axis", 128U, true); !result) return result;
        if (!unique_axes.insert(axis).second) return core::Result<void>::failure(validation("morphology has duplicate axes"));
    }
    for (const auto& [id, structure] : structures_) {
        static_cast<void>(id);
        if (auto result = structure.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("morphology structure"));
        }
        if (auto result = validate_optional_reference(structure.parent_structure,
                                                       "morphology parent structure", structures_); !result) return result;
        if (auto result = validate_reference_list(landmarks_, structure.landmarks,
                                                  "morphology structure landmarks"); !result) return result;
    }
    std::map<StructureId, std::uint8_t> visit;
    const auto visit_structure = [&](const auto& self, StructureId id) -> core::Result<void> {
        const auto state = visit[id];
        if (state == 1U) return core::Result<void>::failure(validation("morphology structure hierarchy contains a cycle"));
        if (state == 2U) return core::Result<void>::success();
        visit[id] = 1U;
        const auto& structure = structures_.at(id);
        if (structure.parent_structure.has_value()) {
            if (auto result = self(self, *structure.parent_structure); !result) return result;
        }
        visit[id] = 2U;
        return core::Result<void>::success();
    };
    for (const auto& [id, structure] : structures_) {
        static_cast<void>(structure);
        if (auto result = visit_structure(visit_structure, id); !result) return result;
    }
    for (const auto& [id, attachment] : attachments_) {
        static_cast<void>(id);
        if (auto result = attachment.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("morphology attachment"));
        }
        if (auto result = validate_reference(structures_, attachment.first_structure,
                                             "morphology attachment first structure"); !result) return result;
        if (auto result = validate_reference(structures_, attachment.second_structure,
                                             "morphology attachment second structure"); !result) return result;
    }
    for (const auto& [id, landmark] : landmarks_) {
        static_cast<void>(id);
        if (auto result = landmark.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("morphology landmark"));
        }
        if (auto result = validate_optional_reference(landmark.structure,
                                                       "morphology landmark structure", structures_); !result) return result;
    }
    for (const auto& [id, growth_domain] : growth_domains_) {
        static_cast<void>(id);
        if (auto result = growth_domain.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("morphology growth domain"));
        }
        if (auto result = validate_reference(structures_, growth_domain.structure,
                                             "morphology growth domain structure"); !result) {
            return result;
        }
    }
    return core::Result<void>::success();
}

std::string MorphologyModel::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTOGRAPHER_MORPHOLOGY " << kSchemaVersion << '\n';
    output << std::quoted(organism_identity) << ' ' << std::quoted(body_plan) << ' '
           << std::quoted(symmetry) << ' ' << axes.size();
    for (const auto& axis : axes) output << ' ' << std::quoted(axis);
    output << '\n';
    output << "STRUCTURES " << structures_.size() << '\n';
    for (const auto& [id, structure] : structures_) {
        output << "STRUCTURE " << id.value << ' ' << optional_id(structure.parent_structure) << ' '
               << std::quoted(structure.semantic_class) << ' ' << std::quoted(structure.laterality) << ' '
               << std::quoted(structure.developmental_origin) << ' ' << std::quoted(structure.geometry.kind) << ' '
               << std::quoted(structure.geometry.reference) << ' ' << std::quoted(structure.local_frame) << ' ';
        write_id_list(output, structure.landmarks);
        output << ' ' << std::quoted(structure.confidence) << ' ';
        write_provenance(output, structure.provenance);
        output << '\n';
    }
    output << "ATTACHMENTS " << attachments_.size() << '\n';
    for (const auto& [id, attachment] : attachments_) {
        output << "ATTACHMENT " << id.value << ' ' << attachment.first_structure.value << ' '
               << attachment.second_structure.value << ' ' << std::quoted(attachment.relationship) << ' ';
        write_provenance(output, attachment.provenance);
        output << '\n';
    }
    output << "LANDMARKS " << landmarks_.size() << '\n';
    for (const auto& [id, landmark] : landmarks_) {
        output << "LANDMARK " << id.value << ' ' << optional_id(landmark.structure) << ' '
               << std::quoted(landmark.name) << ' ' << landmark.position.x << ' '
               << landmark.position.y << ' ' << landmark.position.z << ' '
               << std::quoted(landmark.coordinate_frame) << ' ';
        write_provenance(output, landmark.provenance);
        output << '\n';
    }
    output << "GROWTH_DOMAINS " << growth_domains_.size() << '\n';
    for (const auto& [id, growth_domain] : growth_domains_) {
        output << "GROWTH_DOMAIN " << id.value << ' ' << growth_domain.structure.value << ' '
               << optional_id(growth_domain.driving_field) << ' '
               << growth_domain.growth_direction.x << ' '
               << growth_domain.growth_direction.y << ' '
               << growth_domain.growth_direction.z << ' ' << growth_domain.growth_rate << ' '
               << growth_domain.anisotropy.x << ' ' << growth_domain.anisotropy.y << ' '
               << growth_domain.anisotropy.z << ' '
               << (growth_domain.maximum_extent.has_value() ? 1U : 0U);
        if (growth_domain.maximum_extent.has_value()) {
            output << ' ' << *growth_domain.maximum_extent;
        }
        output << ' ' << std::quoted(growth_domain.start_stage) << ' '
               << std::quoted(growth_domain.end_stage) << ' ';
        write_provenance(output, growth_domain.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<MorphologyModel> MorphologyModel::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<MorphologyModel>::failure(parse_error("morphology record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_MORPHOLOGY"); !result) {
        return core::Result<MorphologyModel>::failure(result.error());
    }
    const auto version = read_uint(input, "morphology schema version");
    if (!version || (version.value() != 1U && version.value() != kSchemaVersion)) {
        return core::Result<MorphologyModel>::failure(Diagnostic(
            ErrorCode::version_mismatch, "unsupported morphology schema version"));
    }
    const auto identity = read_string(input, "morphology organism identity", 256U, true);
    const auto body_plan = read_string(input, "morphology body plan", 256U, true);
    const auto symmetry = read_string(input, "morphology symmetry", 128U);
    const auto axis_count = read_uint(input, "morphology axis count");
    if (!identity || !body_plan || !symmetry || !axis_count || axis_count.value() > 32U) {
        return core::Result<MorphologyModel>::failure(parse_error("invalid morphology header"));
    }
    MorphologyModel model;
    model.organism_identity = identity.value();
    model.body_plan = body_plan.value();
    model.symmetry = symmetry.value();
    for (std::uint64_t index = 0U; index < axis_count.value(); ++index) {
        const auto axis = read_string(input, "morphology axis", 128U, true);
        if (!axis) return core::Result<MorphologyModel>::failure(axis.error());
        model.axes.push_back(axis.value());
    }
    if (auto result = require_record(input, "STRUCTURES"); !result) return core::Result<MorphologyModel>::failure(result.error());
    const auto structure_count = read_uint(input, "morphology structure count");
    if (!structure_count || structure_count.value() > kMaxCollectionEntries) return core::Result<MorphologyModel>::failure(parse_error("invalid morphology structure count"));
    for (std::uint64_t index = 0U; index < structure_count.value(); ++index) {
        if (auto result = require_record(input, "STRUCTURE"); !result) return core::Result<MorphologyModel>::failure(result.error());
        const auto id = read_uint(input, "morphology structure id");
        const auto parent = read_optional_id<StructureId>(input, "morphology parent structure");
        const auto semantic = read_string(input, "morphology semantic class", 256U, true);
        const auto laterality = read_string(input, "morphology laterality", 128U);
        const auto origin = read_string(input, "morphology developmental origin", kMaxReferenceBytes);
        const auto kind = read_string(input, "morphology geometry kind", 128U);
        const auto reference = read_string(input, "morphology geometry reference", kMaxReferenceBytes);
        const auto local_frame = read_string(input, "morphology local frame", 256U);
        const auto landmarks = read_id_list<LandmarkId>(input, "morphology structure landmarks");
        const auto confidence = read_string(input, "morphology confidence", 128U);
        if (!id || !parent || !semantic || !laterality || !origin || !kind || !reference ||
            !local_frame || !landmarks || !confidence) return core::Result<MorphologyModel>::failure(parse_error("invalid morphology structure record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<MorphologyModel>::failure(provenance.error());
        auto inserted = model.insert_structure(MorphologyStructure{
            StructureId{id.value()}, semantic.value(), parent.value(), laterality.value(), origin.value(),
            GeometryBinding{kind.value(), reference.value()}, local_frame.value(), landmarks.value(),
            confidence.value(), provenance.value()});
        if (!inserted) return core::Result<MorphologyModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "ATTACHMENTS"); !result) return core::Result<MorphologyModel>::failure(result.error());
    const auto attachment_count = read_uint(input, "morphology attachment count");
    if (!attachment_count || attachment_count.value() > kMaxCollectionEntries) return core::Result<MorphologyModel>::failure(parse_error("invalid morphology attachment count"));
    for (std::uint64_t index = 0U; index < attachment_count.value(); ++index) {
        if (auto result = require_record(input, "ATTACHMENT"); !result) return core::Result<MorphologyModel>::failure(result.error());
        const auto id = read_uint(input, "morphology attachment id"); const auto first = read_uint(input, "morphology attachment first structure"); const auto second = read_uint(input, "morphology attachment second structure"); const auto relationship = read_string(input, "morphology attachment relationship", 128U, true);
        if (!id || !first || !second || !relationship || first.value() == 0U || second.value() == 0U) return core::Result<MorphologyModel>::failure(parse_error("invalid morphology attachment record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<MorphologyModel>::failure(provenance.error());
        auto inserted = model.insert_attachment(MorphologyAttachment{AttachmentId{id.value()}, StructureId{first.value()}, StructureId{second.value()}, relationship.value(), provenance.value()});
        if (!inserted) return core::Result<MorphologyModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "LANDMARKS"); !result) return core::Result<MorphologyModel>::failure(result.error());
    const auto landmark_count = read_uint(input, "morphology landmark count");
    if (!landmark_count || landmark_count.value() > kMaxCollectionEntries) return core::Result<MorphologyModel>::failure(parse_error("invalid morphology landmark count"));
    for (std::uint64_t index = 0U; index < landmark_count.value(); ++index) {
        if (auto result = require_record(input, "LANDMARK"); !result) return core::Result<MorphologyModel>::failure(result.error());
        const auto id = read_uint(input, "morphology landmark id"); const auto structure = read_optional_id<StructureId>(input, "morphology landmark structure"); const auto name = read_string(input, "morphology landmark name", 256U, true); const auto x = read_double(input, "morphology landmark x"); const auto y = read_double(input, "morphology landmark y"); const auto z = read_double(input, "morphology landmark z"); const auto frame = read_string(input, "morphology landmark coordinate frame", 256U, true);
        if (!id || !structure || !name || !x || !y || !z || !frame) return core::Result<MorphologyModel>::failure(parse_error("invalid morphology landmark record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<MorphologyModel>::failure(provenance.error());
        auto inserted = model.insert_landmark(MorphologyLandmark{LandmarkId{id.value()}, name.value(), structure.value(), {x.value(), y.value(), z.value()}, frame.value(), provenance.value()});
        if (!inserted) return core::Result<MorphologyModel>::failure(inserted.error());
    }
    if (version.value() >= 2U) {
        if (auto result = require_record(input, "GROWTH_DOMAINS"); !result) {
            return core::Result<MorphologyModel>::failure(result.error());
        }
        const auto growth_domain_count = read_uint(input, "morphology growth domain count");
        if (!growth_domain_count || growth_domain_count.value() > kMaxCollectionEntries) {
            return core::Result<MorphologyModel>::failure(parse_error(
                "invalid morphology growth domain count"));
        }
        for (std::uint64_t index = 0U; index < growth_domain_count.value(); ++index) {
            if (auto result = require_record(input, "GROWTH_DOMAIN"); !result) {
                return core::Result<MorphologyModel>::failure(result.error());
            }
            const auto id = read_uint(input, "morphology growth domain id");
            const auto structure = read_uint(input, "morphology growth domain structure");
            const auto driving_field = read_optional_id<FieldId>(
                input, "morphology growth domain driving field");
            const auto direction_x = read_double(input, "morphology growth domain direction x");
            const auto direction_y = read_double(input, "morphology growth domain direction y");
            const auto direction_z = read_double(input, "morphology growth domain direction z");
            const auto rate = read_double(input, "morphology growth domain rate");
            const auto anisotropy_x = read_double(input, "morphology growth domain anisotropy x");
            const auto anisotropy_y = read_double(input, "morphology growth domain anisotropy y");
            const auto anisotropy_z = read_double(input, "morphology growth domain anisotropy z");
            const auto maximum_flag = read_uint(input, "morphology growth domain maximum flag");
            if (!id || !structure || !driving_field || !direction_x || !direction_y ||
                !direction_z || !rate || !anisotropy_x || !anisotropy_y || !anisotropy_z ||
                !maximum_flag || maximum_flag.value() > 1U) {
                return core::Result<MorphologyModel>::failure(parse_error(
                    "invalid morphology growth domain record"));
            }
            std::optional<double> maximum_extent;
            if (maximum_flag.value() != 0U) {
                const auto parsed = read_double(input, "morphology growth domain maximum extent");
                if (!parsed) return core::Result<MorphologyModel>::failure(parsed.error());
                maximum_extent = parsed.value();
            }
            const auto start_stage = read_string(input, "morphology growth domain start stage",
                                                 256U, true);
            const auto end_stage = read_string(input, "morphology growth domain end stage",
                                               256U, true);
            const auto provenance = read_provenance(input);
            if (!start_stage || !end_stage || !provenance) {
                return core::Result<MorphologyModel>::failure(parse_error(
                    "invalid morphology growth domain metadata"));
            }
            auto inserted = model.insert_growth_domain(MorphologyGrowthDomain{
                GrowthDomainId{id.value()}, StructureId{structure.value()}, driving_field.value(),
                {direction_x.value(), direction_y.value(), direction_z.value()}, rate.value(),
                {anisotropy_x.value(), anisotropy_y.value(), anisotropy_z.value()},
                maximum_extent, start_stage.value(), end_stage.value(), provenance.value()});
            if (!inserted) return core::Result<MorphologyModel>::failure(inserted.error());
        }
    }
    if (auto result = require_record(input, "END"); !result) return core::Result<MorphologyModel>::failure(result.error());
    std::string trailing; if (input >> trailing) return core::Result<MorphologyModel>::failure(parse_error("morphology record contains trailing data"));
    if (auto result = model.validate(); !result) return core::Result<MorphologyModel>::failure(result.error());
    return core::Result<MorphologyModel>::success(std::move(model));
}

core::Result<void> MorphometricObservation::validate() const {
    if (auto result = validate_id(id.value, "morphometric observation id"); !result) return result;
    if (auto result = validate_id(structure.value, "morphometric observation structure"); !result) {
        return result;
    }
    if (auto result = validate_text(metric, "morphometric observation metric", 256U, true);
        !result) return result;
    if (auto result = validate_text(units, "morphometric observation units", 128U); !result) {
        return result;
    }
    if (!std::isfinite(value)) {
        return core::Result<void>::failure(validation(
            "morphometric observation value is not finite"));
    }
    if (auto result = validate_text(method, "morphometric observation method", kMaxTextBytes, true);
        !result) return result;
    return provenance.validate();
}

core::Result<void> MorphologySignature::validate() const {
    if (auto result = validate_id(id.value, "morphology signature id"); !result) return result;
    if (auto result = validate_id(structure.value, "morphology signature structure"); !result) {
        return result;
    }
    if (auto result = validate_text(descriptor, "morphology signature descriptor", 256U, true);
        !result) return result;
    if (auto result = validate_text(coordinate_frame, "morphology signature coordinate frame", 256U, true);
        !result) return result;
    if (auto result = validate_text(normalization, "morphology signature normalization", 256U, true);
        !result) return result;
    constexpr std::size_t kMaxComponents = 4096U;
    if (components.size() > kMaxComponents || component_labels.size() > kMaxComponents) {
        return core::Result<void>::failure(validation(
            "morphology signature exceeds the component safety limit"));
    }
    if (!component_labels.empty() && component_labels.size() != components.size()) {
        return core::Result<void>::failure(validation(
            "morphology signature labels must match component count"));
    }
    for (const auto component : components) {
        if (!std::isfinite(component)) {
            return core::Result<void>::failure(validation(
                "morphology signature contains a non-finite component"));
        }
    }
    for (const auto& label : component_labels) {
        if (auto result = validate_text(label, "morphology signature component label", 256U, true);
            !result) return result;
    }
    return provenance.validate();
}

core::Result<void> MorphologyComparison::validate() const {
    if (auto result = validate_id(id.value, "morphology comparison id"); !result) return result;
    if (auto result = validate_id(reference_structure.value, "morphology comparison reference structure");
        !result) return result;
    if (auto result = validate_id(candidate_structure.value, "morphology comparison candidate structure");
        !result) return result;
    if (reference_structure == candidate_structure) {
        return core::Result<void>::failure(validation(
            "morphology comparison structures must be distinct"));
    }
    if (auto result = validate_text(metric, "morphology comparison metric", 256U, true); !result) {
        return result;
    }
    if (!std::isfinite(distance) || distance < 0.0) {
        return core::Result<void>::failure(validation(
            "morphology comparison distance must be finite and non-negative"));
    }
    if (!std::isfinite(similarity) || similarity < 0.0 || similarity > 1.0) {
        return core::Result<void>::failure(validation(
            "morphology comparison similarity must be finite in the [0,1] range"));
    }
    return provenance.validate();
}

core::Result<void> MorphologySignatureComparisonPreview::validate() const {
    if (auto result = validate_id(reference_signature.value,
                                  "morphology signature preview reference signature"); !result) {
        return result;
    }
    if (auto result = validate_id(candidate_signature.value,
                                  "morphology signature preview candidate signature"); !result) {
        return result;
    }
    if (reference_signature == candidate_signature) {
        return core::Result<void>::failure(validation(
            "morphology signature preview signatures must be distinct"));
    }
    if (auto result = validate_id(reference_structure.value,
                                  "morphology signature preview reference structure"); !result) {
        return result;
    }
    if (auto result = validate_id(candidate_structure.value,
                                  "morphology signature preview candidate structure"); !result) {
        return result;
    }
    if (reference_structure == candidate_structure) {
        return core::Result<void>::failure(validation(
            "morphology signature preview structures must be distinct"));
    }
    if (auto result = validate_text(metric, "morphology signature preview metric", 64U, true);
        !result) {
        return result;
    }
    if (metric != "euclidean") {
        return core::Result<void>::failure(validation(
            "morphology signature preview metric is unsupported"));
    }
    if (component_deltas.empty() || component_deltas.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "morphology signature preview deltas are empty or exceed the safety limit"));
    }
    double expected_distance = 0.0;
    for (const double delta : component_deltas) {
        if (!std::isfinite(delta)) {
            return core::Result<void>::failure(validation(
                "morphology signature preview contains a non-finite delta"));
        }
        expected_distance = std::hypot(expected_distance, delta);
    }
    if (!std::isfinite(expected_distance) || !std::isfinite(distance) || distance < 0.0 ||
        std::abs(distance - expected_distance) >
            1.0e-12 * std::max({1.0, std::abs(distance), std::abs(expected_distance)})) {
        return core::Result<void>::failure(validation(
            "morphology signature preview distance does not match its deltas"));
    }
    const double expected_similarity = 1.0 / (1.0 + expected_distance);
    if (!std::isfinite(similarity) || similarity < 0.0 || similarity > 1.0 ||
        std::abs(similarity - expected_similarity) >
            1.0e-12 * std::max({1.0, std::abs(similarity), std::abs(expected_similarity)})) {
        return core::Result<void>::failure(validation(
            "morphology signature preview similarity does not match its distance"));
    }
    return core::Result<void>::success();
}

core::Result<void> EvolutionaryMorphospaceBinding::validate() const {
    if (auto result = validate_id(lineage.value, "morphospace binding lineage"); !result) {
        return result;
    }
    if (auto result = validate_id(signature.value, "morphospace binding signature"); !result) {
        return result;
    }
    return core::Result<void>::success();
}

core::Result<void> EvolutionaryMorphospacePoint::validate() const {
    if (auto result = validate_id(lineage.value, "morphospace point lineage"); !result) {
        return result;
    }
    if (auto result = validate_id(signature.value, "morphospace point signature"); !result) {
        return result;
    }
    if (!position.finite()) {
        return core::Result<void>::failure(validation(
            "morphospace point position must be finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> EvolutionaryMorphospaceEdge::validate() const {
    if (auto result = validate_id(parent.value, "morphospace edge parent"); !result) {
        return result;
    }
    if (auto result = validate_id(child.value, "morphospace edge child"); !result) {
        return result;
    }
    if (parent == child) {
        return core::Result<void>::failure(validation(
            "morphospace edge parent and child must be distinct"));
    }
    if (!displacement.finite()) {
        return core::Result<void>::failure(validation(
            "morphospace edge displacement must be finite"));
    }
    if (!std::isfinite(distance) || distance < 0.0) {
        return core::Result<void>::failure(validation(
            "morphospace edge distance must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

core::Result<void> EvolutionaryMorphospacePreview::validate() const {
    if (auto result = validate_text(projection, "morphospace projection", 64U, true);
        !result) {
        return result;
    }
    if (projection != "leading-components") {
        return core::Result<void>::failure(validation(
            "morphospace projection is unsupported"));
    }
    if (auto result = validate_text(normalization, "morphospace normalization", 64U, true);
        !result) {
        return result;
    }
    if (normalization != "raw" && normalization != "unit-box") {
        return core::Result<void>::failure(validation(
            "morphospace normalization is unsupported"));
    }
    if (dimensions != 2U && dimensions != 3U) {
        return core::Result<void>::failure(validation(
            "morphospace dimensions must be 2 or 3"));
    }
    if (auto result = validate_text(descriptor, "morphospace descriptor", 256U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(coordinate_frame, "morphospace coordinate frame", 256U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(signature_normalization,
                                    "morphospace signature normalization", 256U, true);
        !result) {
        return result;
    }
    if (!component_labels.empty() && component_labels.size() != dimensions) {
        return core::Result<void>::failure(validation(
            "morphospace component labels must match the projected dimensions"));
    }
    for (const auto& label : component_labels) {
        if (auto result = validate_text(label, "morphospace component label", 256U, true);
            !result) {
            return result;
        }
    }
    if (points.empty() || points.size() > kMaxMorphospacePoints) {
        return core::Result<void>::failure(validation(
            "morphospace points are empty or exceed the safety limit"));
    }

    std::map<LineageId, core::Vec3d> positions;
    std::set<SignatureId> signatures;
    for (const auto& point : points) {
        if (auto result = point.validate(); !result) return result;
        if (!positions.emplace(point.lineage, point.position).second) {
            return core::Result<void>::failure(validation(
                "morphospace points contain a duplicate lineage"));
        }
        if (!signatures.insert(point.signature).second) {
            return core::Result<void>::failure(validation(
                "morphospace points contain a duplicate signature"));
        }
        if (dimensions == 2U && std::abs(point.position.z) > 1.0e-12) {
            return core::Result<void>::failure(validation(
                "2D morphospace points must have zero depth"));
        }
        if (normalization == "unit-box" &&
            (point.position.x < 0.0 || point.position.x > 1.0 ||
             point.position.y < 0.0 || point.position.y > 1.0 ||
             (dimensions == 3U &&
              (point.position.z < 0.0 || point.position.z > 1.0)))) {
            return core::Result<void>::failure(validation(
                "unit-box morphospace points must lie in the [0,1] range"));
        }
    }

    std::set<std::pair<LineageId, LineageId>> edge_keys;
    for (const auto& edge : trajectories) {
        if (auto result = edge.validate(); !result) return result;
        if (!positions.contains(edge.parent) || !positions.contains(edge.child)) {
            return core::Result<void>::failure(validation(
                "morphospace trajectory references a point outside the preview"));
        }
        if (!edge_keys.emplace(edge.parent, edge.child).second) {
            return core::Result<void>::failure(validation(
                "morphospace trajectories contain a duplicate edge"));
        }
        const auto expected = positions.at(edge.child) - positions.at(edge.parent);
        const double expected_distance = std::hypot(
            std::hypot(expected.x, expected.y), expected.z);
        const double scale = std::max({1.0, std::abs(edge.distance), expected_distance});
        if (!expected.finite() || !std::isfinite(expected_distance) ||
            std::abs(edge.displacement.x - expected.x) > 1.0e-12 * scale ||
            std::abs(edge.displacement.y - expected.y) > 1.0e-12 * scale ||
            std::abs(edge.displacement.z - expected.z) > 1.0e-12 * scale ||
            std::abs(edge.distance - expected_distance) > 1.0e-12 * scale) {
            return core::Result<void>::failure(validation(
                "morphospace trajectory does not match its endpoint positions"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> MorphometricModel::insert_observation(MorphometricObservation value) {
    return insert_unique(observations_, std::move(value), "morphometric observation");
}

core::Result<void> MorphometricModel::insert_signature(MorphologySignature value) {
    return insert_unique(signatures_, std::move(value), "morphology signature");
}

core::Result<void> MorphometricModel::insert_comparison(MorphologyComparison value) {
    return insert_unique(comparisons_, std::move(value), "morphology comparison");
}

core::Result<MorphologySignatureComparisonPreview>
MorphometricModel::preview_signature_comparison(
    SignatureId reference_signature,
    SignatureId candidate_signature) const {
    if (auto result = validate(); !result) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(
            result.error().with_context("morphology signature comparison preview"));
    }
    if (auto result = validate_id(reference_signature.value,
                                  "morphology signature preview reference signature"); !result) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(result.error());
    }
    if (auto result = validate_id(candidate_signature.value,
                                  "morphology signature preview candidate signature"); !result) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(result.error());
    }
    if (reference_signature == candidate_signature) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(validation(
            "morphology signature preview signatures must be distinct"));
    }
    const auto reference_iterator = signatures_.find(reference_signature);
    const auto candidate_iterator = signatures_.find(candidate_signature);
    if (reference_iterator == signatures_.end() || candidate_iterator == signatures_.end()) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(validation(
            "morphology signature preview references an unknown signature"));
    }
    const auto& reference = reference_iterator->second;
    const auto& candidate = candidate_iterator->second;
    if (reference.structure == candidate.structure) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(validation(
            "morphology signature preview structures must be distinct"));
    }
    if (reference.descriptor != candidate.descriptor ||
        reference.coordinate_frame != candidate.coordinate_frame ||
        reference.normalization != candidate.normalization ||
        reference.component_labels != candidate.component_labels ||
        reference.components.size() != candidate.components.size() ||
        reference.components.empty()) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(validation(
            "morphology signature preview requires matching descriptor, frame, normalization, labels, and component count"));
    }

    MorphologySignatureComparisonPreview preview;
    preview.reference_signature = reference_signature;
    preview.candidate_signature = candidate_signature;
    preview.reference_structure = reference.structure;
    preview.candidate_structure = candidate.structure;
    preview.metric = "euclidean";
    preview.component_deltas.reserve(reference.components.size());
    for (std::size_t index = 0U; index < reference.components.size(); ++index) {
        const double delta = candidate.components[index] - reference.components[index];
        if (!std::isfinite(delta)) {
            return core::Result<MorphologySignatureComparisonPreview>::failure(validation(
                "morphology signature preview delta is not finite"));
        }
        preview.component_deltas.push_back(delta);
        preview.distance = std::hypot(preview.distance, delta);
    }
    if (!std::isfinite(preview.distance)) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(validation(
            "morphology signature preview distance is not finite"));
    }
    preview.similarity = 1.0 / (1.0 + preview.distance);
    if (auto result = preview.validate(); !result) {
        return core::Result<MorphologySignatureComparisonPreview>::failure(result.error());
    }
    return core::Result<MorphologySignatureComparisonPreview>::success(std::move(preview));
}

core::Result<void> MorphometricModel::validate() const {
    if (observations_.size() > kMaxCollectionEntries || signatures_.size() > kMaxCollectionEntries ||
        comparisons_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "morphometric collection exceeds the safety limit"));
    }
    for (const auto& [id, observation] : observations_) {
        static_cast<void>(id);
        if (auto result = observation.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("morphometric observation"));
        }
    }
    for (const auto& [id, signature] : signatures_) {
        static_cast<void>(id);
        if (auto result = signature.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("morphology signature"));
        }
    }
    for (const auto& [id, comparison] : comparisons_) {
        static_cast<void>(id);
        if (auto result = comparison.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("morphology comparison"));
        }
        if (auto result = validate_optional_reference(comparison.reference_signature,
                                                       "morphology comparison reference signature",
                                                       signatures_); !result) return result;
        if (auto result = validate_optional_reference(comparison.candidate_signature,
                                                       "morphology comparison candidate signature",
                                                       signatures_); !result) return result;
        if (comparison.reference_signature.has_value() &&
            signatures_.at(*comparison.reference_signature).structure != comparison.reference_structure) {
            return core::Result<void>::failure(validation(
                "morphology comparison reference signature targets the wrong structure"));
        }
        if (comparison.candidate_signature.has_value() &&
            signatures_.at(*comparison.candidate_signature).structure != comparison.candidate_structure) {
            return core::Result<void>::failure(validation(
                "morphology comparison candidate signature targets the wrong structure"));
        }
    }
    return core::Result<void>::success();
}

std::string MorphometricModel::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTOGRAPHER_MORPHOMETRICS " << kSchemaVersion << '\n';
    output << "OBSERVATIONS " << observations_.size() << '\n';
    for (const auto& [id, observation] : observations_) {
        output << "OBSERVATION " << id.value << ' ' << observation.structure.value << ' '
               << std::quoted(observation.metric) << ' ' << std::quoted(observation.units) << ' '
               << observation.value << ' ' << std::quoted(observation.method) << ' ';
        write_provenance(output, observation.provenance);
        output << '\n';
    }
    output << "SIGNATURES " << signatures_.size() << '\n';
    for (const auto& [id, signature] : signatures_) {
        output << "SIGNATURE " << id.value << ' ' << signature.structure.value << ' '
               << std::quoted(signature.descriptor) << ' ' << std::quoted(signature.coordinate_frame)
               << ' ' << std::quoted(signature.normalization) << ' ' << signature.components.size();
        for (const auto component : signature.components) output << ' ' << component;
        output << ' ' << signature.component_labels.size();
        for (const auto& label : signature.component_labels) output << ' ' << std::quoted(label);
        output << ' ';
        write_provenance(output, signature.provenance);
        output << '\n';
    }
    output << "COMPARISONS " << comparisons_.size() << '\n';
    for (const auto& [id, comparison] : comparisons_) {
        output << "COMPARISON " << id.value << ' ' << comparison.reference_structure.value << ' '
               << comparison.candidate_structure.value << ' ' << std::quoted(comparison.metric) << ' '
               << comparison.distance << ' ' << comparison.similarity << ' '
               << optional_id(comparison.reference_signature) << ' '
               << optional_id(comparison.candidate_signature) << ' ';
        write_provenance(output, comparison.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<MorphometricModel> MorphometricModel::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<MorphometricModel>::failure(
            parse_error("morphometric record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_MORPHOMETRICS"); !result) {
        return core::Result<MorphometricModel>::failure(result.error());
    }
    const auto version = read_uint(input, "morphometric schema version");
    if (!version || (version.value() != 1U && version.value() != kSchemaVersion)) {
        return core::Result<MorphometricModel>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported morphometric schema version"));
    }
    MorphometricModel model;
    if (auto result = require_record(input, "OBSERVATIONS"); !result) {
        return core::Result<MorphometricModel>::failure(result.error());
    }
    const auto observation_count = read_uint(input, "morphometric observation count");
    if (!observation_count || observation_count.value() > kMaxCollectionEntries) {
        return core::Result<MorphometricModel>::failure(parse_error(
            "invalid morphometric observation count"));
    }
    for (std::uint64_t index = 0U; index < observation_count.value(); ++index) {
        if (auto result = require_record(input, "OBSERVATION"); !result) {
            return core::Result<MorphometricModel>::failure(result.error());
        }
        const auto id = read_uint(input, "morphometric observation id");
        const auto structure = read_uint(input, "morphometric observation structure");
        const auto metric = read_string(input, "morphometric observation metric", 256U, true);
        const auto units = read_string(input, "morphometric observation units", 128U);
        const auto value = read_double(input, "morphometric observation value");
        const auto method = read_string(input, "morphometric observation method", kMaxTextBytes, true);
        const auto provenance = read_provenance(input);
        if (!id || !structure || !metric || !units || !value || !method || !provenance) {
            return core::Result<MorphometricModel>::failure(parse_error(
                "invalid morphometric observation record"));
        }
        const auto inserted = model.insert_observation(MorphometricObservation{
            MorphometricId{id.value()}, StructureId{structure.value()}, metric.value(), units.value(),
            value.value(), method.value(), provenance.value()});
        if (!inserted) return core::Result<MorphometricModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "SIGNATURES"); !result) {
        return core::Result<MorphometricModel>::failure(result.error());
    }
    const auto signature_count = read_uint(input, "morphology signature count");
    if (!signature_count || signature_count.value() > kMaxCollectionEntries) {
        return core::Result<MorphometricModel>::failure(parse_error(
            "invalid morphology signature count"));
    }
    constexpr std::size_t kMaxComponents = 4096U;
    for (std::uint64_t index = 0U; index < signature_count.value(); ++index) {
        if (auto result = require_record(input, "SIGNATURE"); !result) {
            return core::Result<MorphometricModel>::failure(result.error());
        }
        const auto id = read_uint(input, "morphology signature id");
        const auto structure = read_uint(input, "morphology signature structure");
        const auto descriptor = read_string(input, "morphology signature descriptor", 256U, true);
        const auto frame = read_string(input, "morphology signature coordinate frame", 256U, true);
        const auto normalization = read_string(input, "morphology signature normalization", 256U, true);
        const auto component_count = read_uint(input, "morphology signature component count");
        if (!id || !structure || !descriptor || !frame || !normalization || !component_count ||
            component_count.value() > kMaxComponents) {
            return core::Result<MorphometricModel>::failure(parse_error(
                "invalid morphology signature header"));
        }
        std::vector<double> components;
        components.reserve(static_cast<std::size_t>(component_count.value()));
        for (std::uint64_t component = 0U; component < component_count.value(); ++component) {
            const auto value = read_double(input, "morphology signature component");
            if (!value) return core::Result<MorphometricModel>::failure(value.error());
            components.push_back(value.value());
        }
        const auto label_count = read_uint(input, "morphology signature label count");
        if (!label_count || label_count.value() > kMaxComponents) {
            return core::Result<MorphometricModel>::failure(parse_error(
                "invalid morphology signature label count"));
        }
        std::vector<std::string> labels;
        labels.reserve(static_cast<std::size_t>(label_count.value()));
        for (std::uint64_t label = 0U; label < label_count.value(); ++label) {
            const auto value = read_string(input, "morphology signature component label", 256U, true);
            if (!value) return core::Result<MorphometricModel>::failure(value.error());
            labels.push_back(value.value());
        }
        const auto provenance = read_provenance(input);
        if (!provenance) return core::Result<MorphometricModel>::failure(provenance.error());
        const auto inserted = model.insert_signature(MorphologySignature{
            SignatureId{id.value()}, StructureId{structure.value()}, descriptor.value(), frame.value(),
            normalization.value(), std::move(components), std::move(labels), provenance.value()});
        if (!inserted) return core::Result<MorphometricModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "COMPARISONS"); !result) {
        return core::Result<MorphometricModel>::failure(result.error());
    }
    const auto comparison_count = read_uint(input, "morphology comparison count");
    if (!comparison_count || comparison_count.value() > kMaxCollectionEntries) {
        return core::Result<MorphometricModel>::failure(parse_error(
            "invalid morphology comparison count"));
    }
    for (std::uint64_t index = 0U; index < comparison_count.value(); ++index) {
        if (auto result = require_record(input, "COMPARISON"); !result) {
            return core::Result<MorphometricModel>::failure(result.error());
        }
        const auto id = read_uint(input, "morphology comparison id");
        const auto reference = read_uint(input, "morphology comparison reference structure");
        const auto candidate = read_uint(input, "morphology comparison candidate structure");
        const auto metric = read_string(input, "morphology comparison metric", 256U, true);
        const auto distance = read_double(input, "morphology comparison distance");
        const auto similarity = read_double(input, "morphology comparison similarity");
        const auto reference_signature = read_optional_id<SignatureId>(
            input, "morphology comparison reference signature");
        const auto candidate_signature = read_optional_id<SignatureId>(
            input, "morphology comparison candidate signature");
        const auto provenance = read_provenance(input);
        if (!id || !reference || !candidate || !metric || !distance || !similarity ||
            !reference_signature || !candidate_signature || !provenance) {
            return core::Result<MorphometricModel>::failure(parse_error(
                "invalid morphology comparison record"));
        }
        const auto inserted = model.insert_comparison(MorphologyComparison{
            ComparisonId{id.value()}, StructureId{reference.value()}, StructureId{candidate.value()},
            metric.value(), distance.value(), similarity.value(), reference_signature.value(),
            candidate_signature.value(), provenance.value()});
        if (!inserted) return core::Result<MorphometricModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<MorphometricModel>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MorphometricModel>::failure(
            parse_error("morphometric record contains trailing data"));
    }
    if (auto result = model.validate(); !result) {
        return core::Result<MorphometricModel>::failure(result.error());
    }
    return core::Result<MorphometricModel>::success(std::move(model));
}

core::Result<void> FunctionalGene::validate() const {
    if (auto result = validate_id(id.value, "gene id"); !result) return result;
    if (auto result = validate_text(name, "gene name", 256U, true); !result) return result;
    if (auto result = validate_text(group, "gene group", 128U, true); !result) return result;
    if (auto result = validate_text(type, "gene type", 128U, true); !result) return result;
    if (!std::isfinite(value)) return core::Result<void>::failure(validation("gene value is not finite"));
    if (auto result = validate_text(units, "gene units", 128U); !result) return result;
    if (lower_bound.has_value() && (!std::isfinite(*lower_bound) || value < *lower_bound)) return core::Result<void>::failure(validation("gene lower bound is invalid"));
    if (upper_bound.has_value() && (!std::isfinite(*upper_bound) || value > *upper_bound)) return core::Result<void>::failure(validation("gene upper bound is invalid"));
    if (lower_bound.has_value() && upper_bound.has_value() && *lower_bound > *upper_bound) return core::Result<void>::failure(validation("gene bounds are inverted"));
    if (auto result = validate_text(mutation_policy, "gene mutation policy", 256U, true); !result) return result;
    if (auto result = validate_text(inheritance_policy, "gene inheritance policy", 256U, true); !result) return result;
    return provenance.validate();
}

core::Result<void> FunctionalGenome::insert_gene(FunctionalGene value) {
    return insert_unique(genes_, std::move(value), "gene");
}

core::Result<void> FunctionalGenome::replace_gene(FunctionalGene value) {
    if (auto result = value.validate(); !result) return result;
    const auto iterator = genes_.find(value.id);
    if (iterator == genes_.end()) {
        return core::Result<void>::failure(invalid(
            "functional genome replacement references an unknown gene"));
    }
    iterator->second = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> FunctionalGenome::remove_gene(GeneId id) {
    if (auto result = validate_id(id.value, "functional genome removal gene id"); !result) {
        return result;
    }
    if (genes_.erase(id) == 0U) {
        return core::Result<void>::failure(invalid(
            "functional genome removal references an unknown gene"));
    }
    return core::Result<void>::success();
}

core::Result<void> FunctionalGenome::validate() const {
    if (auto result = validate_text(schema, "functional genome schema", 128U, true); !result) return result;
    if (auto result = validate_text(developmental_program_reference,
                                    "functional genome developmental program reference",
                                    kMaxReferenceBytes); !result) return result;
    if (auto result = validate_text(lineage_reference,
                                    "functional genome lineage reference",
                                    kMaxReferenceBytes); !result) return result;
    if (auto result = validate_digest(source_digest, "functional genome source digest"); !result) {
        return result;
    }
    for (const auto& [id, gene] : genes_) {
        static_cast<void>(id);
        if (auto result = gene.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("functional gene"));
        }
    }
    return core::Result<void>::success();
}

namespace {

std::string gene_fingerprint(const FunctionalGene& gene) {
    std::ostringstream output;
    output << gene.id.value << ' ' << std::quoted(gene.name) << ' ' << std::quoted(gene.group) << ' '
           << std::quoted(gene.type) << ' ' << std::setprecision(17) << gene.value << ' '
           << std::quoted(gene.units) << ' ' << (gene.lower_bound.has_value() ? 1U : 0U);
    if (gene.lower_bound.has_value()) output << ' ' << *gene.lower_bound;
    output << ' ' << (gene.upper_bound.has_value() ? 1U : 0U);
    if (gene.upper_bound.has_value()) output << ' ' << *gene.upper_bound;
    output << ' ' << std::quoted(gene.mutation_policy) << ' ' << std::quoted(gene.inheritance_policy) << ' ';
    write_provenance(output, gene.provenance);
    return output.str();
}

} // namespace

GenomeDiff FunctionalGenome::diff_against(const FunctionalGenome& parent) const {
    GenomeDiff diff;
    diff.developmental_program_changed =
        developmental_program_reference != parent.developmental_program_reference;
    diff.lineage_changed = lineage_reference != parent.lineage_reference;
    diff.source_identity_changed = source_digest != parent.source_digest;
    std::set<std::string> current_modules;
    std::set<std::string> parent_modules;
    std::set<std::string> changed_modules;
    for (const auto& [id, gene] : genes_) {
        current_modules.insert(gene.group);
        const auto previous = parent.genes_.find(id);
        if (previous == parent.genes_.end()) diff.added.push_back(id);
        else if (gene_fingerprint(gene) != gene_fingerprint(previous->second)) {
            diff.changed.push_back(id);
            changed_modules.insert(gene.group);
            changed_modules.insert(previous->second.group);
            if ((gene.group == "neural" || previous->second.group == "neural") &&
                gene.value != previous->second.value) {
                diff.changed_neural_values.push_back(id);
            }
        }
    }
    for (const auto& [id, gene] : parent.genes_) {
        parent_modules.insert(gene.group);
        if (!genes_.contains(id)) diff.removed.push_back(id);
    }
    std::set_difference(current_modules.begin(), current_modules.end(),
                        parent_modules.begin(), parent_modules.end(),
                        std::back_inserter(diff.added_modules));
    std::set_difference(parent_modules.begin(), parent_modules.end(),
                        current_modules.begin(), current_modules.end(),
                        std::back_inserter(diff.removed_modules));
    diff.changed_modules.assign(changed_modules.begin(), changed_modules.end());
    return diff;
}

std::string FunctionalGenome::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_FUNCTIONAL_GENOME " << kSchemaVersion << '\n';
    output << std::quoted(schema) << ' ' << std::quoted(developmental_program_reference) << ' '
           << std::quoted(lineage_reference);
    write_optional_digest(output, source_digest);
    output << '\n';
    output << "GENES " << genes_.size() << '\n';
    for (const auto& [id, gene] : genes_) {
        output << "GENE " << id.value << ' ' << std::quoted(gene.name) << ' '
               << std::quoted(gene.group) << ' ' << std::quoted(gene.type) << ' '
               << std::setprecision(17) << gene.value << ' ' << std::quoted(gene.units) << ' '
               << (gene.lower_bound.has_value() ? 1U : 0U);
        if (gene.lower_bound.has_value()) output << ' ' << *gene.lower_bound;
        output << ' ' << (gene.upper_bound.has_value() ? 1U : 0U);
        if (gene.upper_bound.has_value()) output << ' ' << *gene.upper_bound;
        output << ' ' << std::quoted(gene.mutation_policy) << ' ' << std::quoted(gene.inheritance_policy) << ' ';
        write_provenance(output, gene.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<FunctionalGenome> FunctionalGenome::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) return core::Result<FunctionalGenome>::failure(parse_error("functional genome record is too large"));
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_FUNCTIONAL_GENOME"); !result) return core::Result<FunctionalGenome>::failure(result.error());
    const auto version = read_uint(input, "functional genome schema version");
    if (!version || (version.value() != 1U && version.value() != kSchemaVersion)) {
        return core::Result<FunctionalGenome>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported functional genome schema version"));
    }
    const auto schema = read_string(input, "functional genome schema", 128U, true); const auto program = read_string(input, "functional genome developmental program reference", kMaxReferenceBytes);
    if (!schema || !program) return core::Result<FunctionalGenome>::failure(parse_error("invalid functional genome header"));
    FunctionalGenome genome;
    genome.schema = schema.value();
    genome.developmental_program_reference = program.value();
    if (version.value() >= 2U) {
        const auto lineage = read_string(input, "functional genome lineage reference", kMaxReferenceBytes);
        const auto source_digest = read_optional_digest(input, "functional genome source");
        if (!lineage || !source_digest) {
            return core::Result<FunctionalGenome>::failure(parse_error(
                "invalid functional genome provenance header"));
        }
        genome.lineage_reference = lineage.value();
        genome.source_digest = source_digest.value();
    }
    if (auto result = require_record(input, "GENES"); !result) return core::Result<FunctionalGenome>::failure(result.error());
    const auto count = read_uint(input, "functional gene count");
    if (!count || count.value() > kMaxCollectionEntries) return core::Result<FunctionalGenome>::failure(parse_error("invalid functional gene count"));
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "GENE"); !result) return core::Result<FunctionalGenome>::failure(result.error());
        const auto id = read_uint(input, "gene id"); const auto name = read_string(input, "gene name", 256U, true); const auto group = read_string(input, "gene group", 128U, true); const auto type = read_string(input, "gene type", 128U, true); const auto value = read_double(input, "gene value"); const auto units = read_string(input, "gene units", 128U); const auto lower_flag = read_uint(input, "gene lower flag");
        if (!id || !name || !group || !type || !value || !units || !lower_flag || lower_flag.value() > 1U) return core::Result<FunctionalGenome>::failure(parse_error("invalid functional gene record"));
        std::optional<double> lower; if (lower_flag.value() != 0U) { const auto parsed = read_double(input, "gene lower bound"); if (!parsed) return core::Result<FunctionalGenome>::failure(parsed.error()); lower = parsed.value(); }
        const auto upper_flag = read_uint(input, "gene upper flag"); if (!upper_flag || upper_flag.value() > 1U) return core::Result<FunctionalGenome>::failure(parse_error("invalid gene upper flag"));
        std::optional<double> upper; if (upper_flag.value() != 0U) { const auto parsed = read_double(input, "gene upper bound"); if (!parsed) return core::Result<FunctionalGenome>::failure(parsed.error()); upper = parsed.value(); }
        const auto mutation = read_string(input, "gene mutation policy", 256U, true); const auto inheritance = read_string(input, "gene inheritance policy", 256U, true); const auto provenance = read_provenance(input);
        if (!mutation || !inheritance || !provenance) return core::Result<FunctionalGenome>::failure(parse_error("invalid functional gene metadata"));
        auto inserted = genome.insert_gene(FunctionalGene{GeneId{id.value()}, name.value(), group.value(), type.value(), value.value(), units.value(), lower, upper, mutation.value(), inheritance.value(), provenance.value()});
        if (!inserted) return core::Result<FunctionalGenome>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) return core::Result<FunctionalGenome>::failure(result.error());
    std::string trailing; if (input >> trailing) return core::Result<FunctionalGenome>::failure(parse_error("functional genome record contains trailing data"));
    if (auto result = genome.validate(); !result) return core::Result<FunctionalGenome>::failure(result.error());
    return core::Result<FunctionalGenome>::success(std::move(genome));
}

namespace {

bool is_iupac_base(char value) {
    constexpr std::string_view kIupacBases = "ACGTNRYWSKMBDHV";
    return kIupacBases.find(value) != std::string_view::npos;
}

core::Result<void> validate_sequence(std::string_view sequence, std::string_view field) {
    if (sequence.size() > SequenceGenome::kMaxSequenceBytes) {
        return core::Result<void>::failure(validation(
            std::string(field) + " exceeds the sequence safety limit"));
    }
    for (const char base : sequence) {
        if (!is_iupac_base(base)) {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains a non-IUPAC uppercase base"));
        }
    }
    return core::Result<void>::success();
}

std::string encode_fasta_name(std::string_view name) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(name.size() * 2U);
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(kHex[(byte >> 4U) & 0x0fU]);
        encoded.push_back(kHex[byte & 0x0fU]);
    }
    return encoded;
}

core::Result<std::string> decode_fasta_name(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 2U != 0U || encoded.size() > 512U) {
        return core::Result<std::string>::failure(parse_error(
            "FASTA canonical contig name encoding is invalid"));
    }
    const auto hex_value = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2U);
    for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
        const int high = hex_value(encoded[index]);
        const int low = hex_value(encoded[index + 1U]);
        if (high < 0 || low < 0) {
            return core::Result<std::string>::failure(parse_error(
                "FASTA canonical contig name contains a non-hex byte"));
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    if (auto result = validate_text(decoded, "FASTA contig name", 256U, true); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    return core::Result<std::string>::success(std::move(decoded));
}

assets::Sha256Digest sequence_content_digest(std::string_view sequence) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(sequence.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, sequence.size()});
}

bool is_vcf_base(char value) {
    return value == 'A' || value == 'C' || value == 'G' || value == 'T' || value == 'N';
}

core::Result<void> validate_vcf_sequence(std::string_view sequence, std::string_view field) {
    if (sequence.empty() || sequence.size() > SequenceGenome::kMaxSequenceBytes) {
        return core::Result<void>::failure(validation(
            std::string(field) + " is empty or exceeds the sequence safety limit"));
    }
    for (const char base : sequence) {
        if (!is_vcf_base(base)) {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains a non-ACGTN uppercase base"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_vcf_token(
    std::string_view value,
    std::string_view field,
    std::size_t limit,
    bool required = true) {
    if (value.empty() && !required) return core::Result<void>::success();
    if (value.empty() || value.size() > limit) {
        return core::Result<void>::failure(validation(
            std::string(field) + " is empty or exceeds the VCF token limit"));
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte == 0x7fU || character == ';' || character == '=' ||
            character == ',' || character == '\r' || character == '\n') {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains a VCF delimiter or control byte"));
        }
    }
    return core::Result<void>::success();
}

std::string encode_hex_text(std::string_view value) {
    return encode_fasta_name(value);
}

core::Result<std::string> decode_hex_text(
    std::string_view encoded,
    std::string_view field,
    std::size_t limit,
    bool required) {
    if (encoded.empty() && !required) return core::Result<std::string>::success({});
    if (encoded.empty() || encoded.size() % 2U != 0U || encoded.size() > limit * 2U) {
        return core::Result<std::string>::failure(parse_error(
            std::string(field) + " hex value is malformed"));
    }
    const auto hex_value = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2U);
    for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
        const int high = hex_value(encoded[index]);
        const int low = hex_value(encoded[index + 1U]);
        if (high < 0 || low < 0) {
            return core::Result<std::string>::failure(parse_error(
                std::string(field) + " contains a non-hex byte"));
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    if (auto result = validate_text(decoded, field, limit, required); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    return core::Result<std::string>::success(std::move(decoded));
}

std::vector<std::string_view> split_vcf_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0U;
    while (true) {
        const auto separator = line.find('\t', start);
        if (separator == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, separator - start));
        start = separator + 1U;
    }
    return fields;
}

std::uint64_t stable_variant_id(std::string_view key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : key) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

std::string variant_identity_key(
    std::string_view contig,
    std::uint64_t position,
    std::string_view reference,
    std::string_view alternate,
    std::string_view source_identifier) {
    std::string key;
    key.reserve(contig.size() + reference.size() + alternate.size() + source_identifier.size() + 64U);
    key.append(contig);
    key.push_back('\0');
    key.append(std::to_string(position));
    key.push_back('\0');
    key.append(reference);
    key.push_back('\0');
    key.append(alternate);
    key.push_back('\0');
    key.append(source_identifier);
    return key;
}

core::Result<void> validate_gff_token(
    std::string_view value,
    std::string_view field,
    std::size_t limit,
    bool required = true) {
    if (value.empty() && !required) return core::Result<void>::success();
    if (value.empty() || value.size() > limit) {
        return core::Result<void>::failure(validation(
            std::string(field) + " is empty or exceeds the GFF3 token limit"));
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte == 0x7fU || character == ';' || character == '=' ||
            character == ',' || character == '%' || character == '\r' || character == '\n') {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains a GFF3 delimiter or control byte"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_bed_token(
    std::string_view value,
    std::string_view field,
    std::size_t limit,
    bool required = true) {
    if (value.empty() && !required) return core::Result<void>::success();
    if (value.empty() || value.size() > limit) {
        return core::Result<void>::failure(validation(
            std::string(field) + " is empty or exceeds the BED token limit"));
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte == 0x7fU || character == '\r' || character == '\n') {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains a BED delimiter or control byte"));
        }
    }
    return core::Result<void>::success();
}

std::string gtf_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '"') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

core::Result<std::string> gtf_unescape(
    std::string_view value,
    std::string_view field,
    std::size_t limit,
    bool required = false) {
    if (value.empty() && required) {
        return core::Result<std::string>::failure(parse_error(
            std::string(field) + " is empty"));
    }
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (value[index] != '\\') {
            decoded.push_back(value[index]);
            continue;
        }
        if (index + 1U >= value.size() ||
            (value[index + 1U] != '\\' && value[index + 1U] != '"')) {
            return core::Result<std::string>::failure(parse_error(
                std::string(field) + " contains an unsupported escape"));
        }
        decoded.push_back(value[index + 1U]);
        ++index;
    }
    if (auto result = validate_text(decoded, field, limit, required); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    return core::Result<std::string>::success(std::move(decoded));
}

std::string gff3_escape(std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool keep =
            (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || character == '.' || character == '_' ||
            character == ':' || character == '-' || character == '+' || character == '|';
        if (keep) {
            escaped.push_back(character);
        } else {
            escaped.push_back('%');
            escaped.push_back(kHex[(byte >> 4U) & 0x0fU]);
            escaped.push_back(kHex[byte & 0x0fU]);
        }
    }
    return escaped;
}

core::Result<std::string> gff3_unescape(
    std::string_view value,
    std::string_view field,
    std::size_t limit,
    bool required = false) {
    if (value.empty() && required) {
        return core::Result<std::string>::failure(parse_error(
            std::string(field) + " is empty"));
    }
    std::string decoded;
    decoded.reserve(value.size());
    const auto hex_value = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (value[index] != '%') {
            decoded.push_back(value[index]);
            continue;
        }
        if (index + 2U >= value.size()) {
            return core::Result<std::string>::failure(parse_error(
                std::string(field) + " has a truncated percent escape"));
        }
        const int high = hex_value(value[index + 1U]);
        const int low = hex_value(value[index + 2U]);
        if (high < 0 || low < 0) {
            return core::Result<std::string>::failure(parse_error(
                std::string(field) + " has an invalid percent escape"));
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2U;
    }
    if (auto result = validate_text(decoded, field, limit, required); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    return core::Result<std::string>::success(std::move(decoded));
}

core::Result<double> parse_gff_double(
    std::string_view token,
    std::string_view field) {
    std::istringstream input{std::string(token)};
    double value = 0.0;
    std::string trailing;
    if (!(input >> value) || !std::isfinite(value) || (input >> trailing)) {
        return core::Result<double>::failure(parse_error(
            std::string(field) + " is not a finite decimal value"));
    }
    return core::Result<double>::success(value);
}

std::uint64_t stable_feature_id(std::string_view key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : key) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

std::string feature_identity_key(
    std::string_view contig,
    std::uint64_t start,
    std::uint64_t end,
    std::string_view type,
    std::string_view source_identifier,
    std::string_view attributes) {
    std::string key;
    key.reserve(contig.size() + type.size() + source_identifier.size() + attributes.size() + 96U);
    key.append(contig);
    key.push_back('\0');
    key.append(std::to_string(start));
    key.push_back('\0');
    key.append(std::to_string(end));
    key.push_back('\0');
    key.append(type);
    key.push_back('\0');
    key.append(source_identifier);
    key.push_back('\0');
    key.append(attributes);
    return key;
}

} // namespace

core::Result<void> SequenceContig::validate() const {
    if (auto result = validate_id(id.value, "sequence contig id"); !result) return result;
    if (auto result = validate_text(name, "sequence contig name", 256U, true); !result) return result;
    if (auto result = validate_sequence(sequence, "sequence contig sequence"); !result) return result;
    if (sequence.empty() && !content_digest.has_value()) {
        return core::Result<void>::failure(validation(
            "sequence contig must provide sequence bytes or a content digest"));
    }
    if (auto result = validate_digest(content_digest, "sequence contig content digest"); !result) {
        return result;
    }
    return provenance.validate();
}

core::Result<void> SequenceVariant::validate() const {
    if (auto result = validate_id(id.value, "sequence variant id"); !result) return result;
    if (auto result = validate_id(contig.value, "sequence variant contig"); !result) return result;
    if (auto result = validate_sequence(reference, "sequence variant reference"); !result) return result;
    if (auto result = validate_sequence(alternate, "sequence variant alternate"); !result) return result;
    if (reference.empty() || alternate.empty()) {
        return core::Result<void>::failure(validation(
            "sequence variant reference and alternate must be non-empty"));
    }
    if (auto result = validate_text(kind, "sequence variant kind", 128U, true); !result) return result;
    if (auto result = validate_text(source_identifier, "sequence variant source identifier", 256U);
        !result) return result;
    return provenance.validate();
}

core::Result<void> SequenceFeatureAttribute::validate() const {
    if (auto result = validate_text(key, "sequence feature attribute key", 128U, true);
        !result) {
        return result;
    }
    if (key == "ID" || key == "CARTOGRAPHER_ID") {
        return core::Result<void>::failure(validation(
            "sequence feature attribute key is reserved by the Cartographer GFF3 adapter"));
    }
    return validate_text(value, "sequence feature attribute value", 16U * 1024U);
}

core::Result<void> SequenceFeature::validate() const {
    if (auto result = validate_id(id.value, "sequence feature id"); !result) return result;
    if (auto result = validate_id(contig.value, "sequence feature contig"); !result) return result;
    if (start >= end) {
        return core::Result<void>::failure(validation(
            "sequence feature interval must be non-empty and half-open"));
    }
    if (auto result = validate_text(type, "sequence feature type", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_text(source, "sequence feature source", 256U); !result) {
        return result;
    }
    if (score.has_value() && !std::isfinite(*score)) {
        return core::Result<void>::failure(validation(
            "sequence feature score must be finite"));
    }
    if (strand != "." && strand != "+" && strand != "-" && strand != "?") {
        return core::Result<void>::failure(validation(
            "sequence feature strand must be one of ., +, -, or ?"));
    }
    if (phase.has_value() && *phase > 2U) {
        return core::Result<void>::failure(validation(
            "sequence feature phase must be between zero and two"));
    }
    if (auto result = validate_text(source_identifier, "sequence feature source identifier", 256U);
        !result) {
        return result;
    }
    if (attributes.size() > kMaxTags) {
        return core::Result<void>::failure(validation(
            "sequence feature attributes exceed the safety limit"));
    }
    std::set<std::string> attribute_keys;
    for (const auto& attribute : attributes) {
        if (auto result = attribute.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "sequence feature attribute"));
        }
        if (!attribute_keys.insert(attribute.key).second) {
            return core::Result<void>::failure(validation(
                "sequence feature attributes contain a duplicate key"));
        }
    }
    return provenance.validate();
}

core::Result<void> SequenceGenome::insert_contig(SequenceContig value) {
    return insert_unique(contigs_, std::move(value), "sequence contig");
}

core::Result<void> SequenceGenome::insert_variant(SequenceVariant value) {
    return insert_unique(variants_, std::move(value), "sequence variant");
}

core::Result<void> SequenceGenome::insert_feature(SequenceFeature value) {
    return insert_unique(features_, std::move(value), "sequence feature");
}

core::Result<void> SequenceGenome::validate() const {
    if (auto result = validate_text(schema, "sequence genome schema", 128U, true); !result) return result;
    if (auto result = validate_text(assembly, "sequence genome assembly", 256U, true); !result) return result;
    if (auto result = validate_text(sample_reference, "sequence genome sample reference",
                                    kMaxReferenceBytes); !result) return result;
    if (auto result = validate_digest(source_digest, "sequence genome source digest"); !result) return result;
    if (contigs_.size() > kMaxCollectionEntries || variants_.size() > kMaxCollectionEntries ||
        features_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "sequence genome collection exceeds the safety limit"));
    }
    std::size_t sequence_bytes = 0U;
    for (const auto& [id, contig] : contigs_) {
        static_cast<void>(id);
        if (auto result = contig.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("sequence contig"));
        }
        if (sequence_bytes > SequenceGenome::kMaxSequenceBytes - contig.sequence.size()) {
            return core::Result<void>::failure(validation(
                "sequence genome aggregate sequence exceeds the safety limit"));
        }
        sequence_bytes += contig.sequence.size();
    }
    for (const auto& [id, variant] : variants_) {
        static_cast<void>(id);
        if (auto result = variant.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("sequence variant"));
        }
        if (auto result = validate_reference(contigs_, variant.contig, "sequence variant contig"); !result) {
            return result;
        }
        const auto& contig = contigs_.at(variant.contig);
        if (!contig.sequence.empty()) {
            if (variant.position > contig.sequence.size() ||
                variant.reference.size() > contig.sequence.size() - variant.position) {
                return core::Result<void>::failure(validation(
                    "sequence variant reference lies outside its contig"));
            }
            if (contig.sequence.compare(static_cast<std::size_t>(variant.position),
                                        variant.reference.size(), variant.reference) != 0) {
                return core::Result<void>::failure(validation(
                    "sequence variant reference does not match its contig"));
            }
        }
    }
    std::set<std::string> feature_source_identifiers;
    for (const auto& [id, feature] : features_) {
        static_cast<void>(id);
        if (auto result = feature.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("sequence feature"));
        }
        if (auto result = validate_reference(contigs_, feature.contig, "sequence feature contig");
            !result) {
            return result;
        }
        const auto& contig = contigs_.at(feature.contig);
        if (!contig.sequence.empty() && feature.end > contig.sequence.size()) {
            return core::Result<void>::failure(validation(
                "sequence feature interval lies outside its contig"));
        }
        if (!feature.source_identifier.empty() &&
            !feature_source_identifiers.insert(feature.source_identifier).second) {
            return core::Result<void>::failure(validation(
                "sequence feature source identifiers must be unique"));
        }
    }
    return core::Result<void>::success();
}

std::string SequenceGenome::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_SEQUENCE_GENOME " << kSchemaVersion << '\n';
    output << std::quoted(schema) << ' ' << std::quoted(assembly) << ' '
           << std::quoted(sample_reference);
    write_optional_digest(output, source_digest);
    output << '\n';
    output << "CONTIGS " << contigs_.size() << '\n';
    for (const auto& [id, contig] : contigs_) {
        output << "CONTIG " << id.value << ' ' << std::quoted(contig.name) << ' '
               << std::quoted(contig.sequence);
        write_optional_digest(output, contig.content_digest);
        output << ' ';
        write_provenance(output, contig.provenance);
        output << '\n';
    }
    output << "VARIANTS " << variants_.size() << '\n';
    for (const auto& [id, variant] : variants_) {
        output << "VARIANT " << id.value << ' ' << variant.contig.value << ' '
               << variant.position << ' ' << std::quoted(variant.reference) << ' '
               << std::quoted(variant.alternate) << ' ' << std::quoted(variant.kind) << ' ';
        write_provenance(output, variant.provenance);
        output << ' ' << std::quoted(variant.source_identifier);
        output << '\n';
    }
    output << "FEATURES " << features_.size() << '\n';
    output << std::setprecision(17);
    for (const auto& [id, feature] : features_) {
        output << "FEATURE " << id.value << ' ' << feature.contig.value << ' '
               << feature.start << ' ' << feature.end << ' ' << std::quoted(feature.type) << ' '
               << std::quoted(feature.source) << ' ' << (feature.score.has_value() ? 1U : 0U)
               << ' ' << feature.score.value_or(0.0) << ' ' << std::quoted(feature.strand) << ' '
               << (feature.phase.has_value() ? 1U : 0U) << ' '
               << static_cast<unsigned>(feature.phase.value_or(0U)) << ' '
               << std::quoted(feature.source_identifier) << ' ' << feature.attributes.size();
        for (const auto& attribute : feature.attributes) {
            output << ' ' << std::quoted(attribute.key) << ' ' << std::quoted(attribute.value);
        }
        output << ' ';
        write_provenance(output, feature.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<SequenceGenome> SequenceGenome::deserialize(std::string_view text) {
    if (text.size() > SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "sequence genome record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_SEQUENCE_GENOME"); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    const auto version = read_uint(input, "sequence genome schema version");
    if (!version || version.value() < 1U || version.value() > kSchemaVersion) {
        return core::Result<SequenceGenome>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported sequence genome schema version"));
    }
    const auto schema = read_string(input, "sequence genome schema", 128U, true);
    const auto assembly = read_string(input, "sequence genome assembly", 256U, true);
    const auto sample = read_string(input, "sequence genome sample reference", kMaxReferenceBytes);
    const auto source_digest = read_optional_digest(input, "sequence genome source");
    if (!schema || !assembly || !sample || !source_digest) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "invalid sequence genome header"));
    }
    SequenceGenome genome;
    genome.schema = schema.value();
    genome.assembly = assembly.value();
    genome.sample_reference = sample.value();
    genome.source_digest = source_digest.value();
    if (auto result = require_record(input, "CONTIGS"); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    const auto contig_count = read_uint(input, "sequence contig count");
    if (!contig_count || contig_count.value() > kMaxCollectionEntries) {
        return core::Result<SequenceGenome>::failure(parse_error("invalid sequence contig count"));
    }
    for (std::uint64_t index = 0U; index < contig_count.value(); ++index) {
        if (auto result = require_record(input, "CONTIG"); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        const auto id = read_uint(input, "sequence contig id");
        const auto name = read_string(input, "sequence contig name", 256U, true);
        const auto sequence = read_string(input, "sequence contig sequence", SequenceGenome::kMaxSequenceBytes);
        const auto digest = read_optional_digest(input, "sequence contig content");
        const auto provenance = read_provenance(input);
        if (!id || !name || !sequence || !digest || !provenance) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "invalid sequence contig record"));
        }
        const auto inserted = genome.insert_contig(SequenceContig{
            ContigId{id.value()}, name.value(), sequence.value(), digest.value(), provenance.value()});
        if (!inserted) return core::Result<SequenceGenome>::failure(inserted.error());
    }
    if (auto result = require_record(input, "VARIANTS"); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    const auto variant_count = read_uint(input, "sequence variant count");
    if (!variant_count || variant_count.value() > kMaxCollectionEntries) {
        return core::Result<SequenceGenome>::failure(parse_error("invalid sequence variant count"));
    }
    for (std::uint64_t index = 0U; index < variant_count.value(); ++index) {
        if (auto result = require_record(input, "VARIANT"); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        const auto id = read_uint(input, "sequence variant id");
        const auto contig = read_uint(input, "sequence variant contig");
        const auto position = read_uint(input, "sequence variant position");
        const auto reference = read_string(input, "sequence variant reference", SequenceGenome::kMaxSequenceBytes, true);
        const auto alternate = read_string(input, "sequence variant alternate", SequenceGenome::kMaxSequenceBytes, true);
        const auto kind = read_string(input, "sequence variant kind", 128U, true);
        const auto provenance = read_provenance(input);
        const auto source_identifier = version.value() >= 2U
            ? read_string(input, "sequence variant source identifier", 256U)
            : core::Result<std::string>::success({});
        if (!id || !contig || !position || !reference || !alternate || !kind || !provenance ||
            !source_identifier) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "invalid sequence variant record"));
        }
        const auto inserted = genome.insert_variant(SequenceVariant{
            VariantId{id.value()}, ContigId{contig.value()}, position.value(), reference.value(),
            alternate.value(), kind.value(), provenance.value(), source_identifier.value()});
        if (!inserted) return core::Result<SequenceGenome>::failure(inserted.error());
    }
    if (version.value() >= 3U) {
        if (auto result = require_record(input, "FEATURES"); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        const auto feature_count = read_uint(input, "sequence feature count");
        if (!feature_count || feature_count.value() > kMaxCollectionEntries) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "invalid sequence feature count"));
        }
        for (std::uint64_t index = 0U; index < feature_count.value(); ++index) {
            if (auto result = require_record(input, "FEATURE"); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
            const auto id = read_uint(input, "sequence feature id");
            const auto contig = read_uint(input, "sequence feature contig");
            const auto start = read_uint(input, "sequence feature start");
            const auto end = read_uint(input, "sequence feature end");
            const auto type = read_string(input, "sequence feature type", 256U, true);
            const auto source = read_string(input, "sequence feature source", 256U);
            const auto score_present = read_uint(input, "sequence feature score presence");
            const auto score = read_double(input, "sequence feature score");
            const auto strand = read_string(input, "sequence feature strand", 1U, true);
            const auto phase_present = read_uint(input, "sequence feature phase presence");
            const auto phase = read_uint(input, "sequence feature phase");
            const auto source_identifier = read_string(
                input, "sequence feature source identifier", 256U);
            const auto attribute_count = read_uint(input, "sequence feature attribute count");
            if (!id || !contig || !start || !end || !type || !source || !score_present ||
                !score || !strand || !phase_present || !phase || !source_identifier ||
                !attribute_count || score_present.value() > 1U || phase_present.value() > 1U ||
                attribute_count.value() > kMaxTags) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "invalid sequence feature record"));
            }
            std::vector<SequenceFeatureAttribute> attributes;
            attributes.reserve(static_cast<std::size_t>(attribute_count.value()));
            for (std::uint64_t attribute_index = 0U;
                 attribute_index < attribute_count.value(); ++attribute_index) {
                const auto key = read_string(input, "sequence feature attribute key", 128U, true);
                const auto value = read_string(input, "sequence feature attribute value", 16U * 1024U);
                if (!key || !value) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "invalid sequence feature attribute"));
                }
                attributes.push_back({key.value(), value.value()});
            }
            const auto provenance = read_provenance(input);
            if (!provenance || phase.value() > 2U) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "invalid sequence feature provenance or phase"));
            }
            const auto inserted = genome.insert_feature(SequenceFeature{
                SequenceFeatureId{id.value()}, ContigId{contig.value()}, start.value(), end.value(),
                type.value(), source.value(), score_present.value() != 0U
                    ? std::optional<double>{score.value()} : std::nullopt,
                strand.value(), phase_present.value() != 0U
                    ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(phase.value())}
                    : std::nullopt,
                source_identifier.value(), std::move(attributes), provenance.value()});
            if (!inserted) return core::Result<SequenceGenome>::failure(inserted.error());
        }
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SequenceGenome>::failure(
            parse_error("sequence genome record contains trailing data"));
    }
    if (auto result = genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    return core::Result<SequenceGenome>::success(std::move(genome));
}

core::Result<SequenceGenomeFastaExport> SequenceGenome::export_fasta(
    std::size_t line_width) const {
    if (auto result = validate(); !result) {
        return core::Result<SequenceGenomeFastaExport>::failure(
            result.error().with_context("FASTA export"));
    }
    constexpr std::size_t kMaxFastaTextBytes = SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    if (line_width == 0U || line_width > 4096U) {
        return core::Result<SequenceGenomeFastaExport>::failure(validation(
            "FASTA line width is outside the safety range"));
    }

    std::ostringstream output;
    for (const auto& [id, contig] : contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenomeFastaExport>::failure(validation(
                "FASTA export cannot represent a digest-only contig without sequence bytes"));
        }
        output << ">cartographer.sequence.v1|id=" << id.value
               << "|name_hex=" << encode_fasta_name(contig.name) << '\n';
        for (std::size_t offset = 0U; offset < contig.sequence.size(); offset += line_width) {
            output << contig.sequence.substr(offset, line_width) << '\n';
        }
        if (output.tellp() < 0 || static_cast<std::size_t>(output.tellp()) > kMaxFastaTextBytes) {
            return core::Result<SequenceGenomeFastaExport>::failure(validation(
                "FASTA export exceeds the safety limit"));
        }
    }
    SequenceGenomeFastaExport result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxFastaTextBytes) {
        return core::Result<SequenceGenomeFastaExport>::failure(validation(
            "FASTA export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "FASTA omits Cartographer assembly, sample, source-digest, and per-record provenance metadata");
    if (variants_.empty()) {
        result.feature_loss_notes.push_back("FASTA carries no explicit sequence variants");
    } else {
        result.feature_loss_notes.push_back(
            "FASTA omits " + std::to_string(variants_.size()) + " explicit sequence variants");
    }
    return core::Result<SequenceGenomeFastaExport>::success(std::move(result));
}

core::Result<SequenceGenome> SequenceGenome::import_fasta(
    std::string_view text,
    std::string assembly,
    std::string sample_reference,
    Provenance provenance,
    std::optional<assets::Sha256Digest> source_digest) {
    constexpr std::size_t kMaxFastaTextBytes = SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    if (text.empty() || text.size() > kMaxFastaTextBytes) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "FASTA input is empty or exceeds the safety limit"));
    }
    SequenceGenome genome;
    genome.schema = "cartographer.sequence-genome.v1";
    genome.assembly = std::move(assembly);
    genome.sample_reference = std::move(sample_reference);
    genome.source_digest = source_digest;

    std::istringstream input{std::string(text)};
    std::string line;
    std::string name;
    std::string sequence;
    ContigId id{};
    std::uint64_t next_id = 1U;
    bool active = false;
    const auto flush = [&]() -> core::Result<void> {
        if (!active) return core::Result<void>::success();
        if (sequence.empty()) {
            return core::Result<void>::failure(parse_error(
                "FASTA record has no sequence bytes"));
        }
        const auto inserted = genome.insert_contig(SequenceContig{
            id, name, sequence, sequence_content_digest(sequence), provenance});
        if (!inserted) return inserted;
        active = false;
        name.clear();
        sequence.clear();
        return core::Result<void>::success();
    };

    constexpr std::string_view kCanonicalPrefix = "cartographer.sequence.v1|id=";
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 4096U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "FASTA line exceeds the safety limit"));
        }
        if (line.empty()) continue;
        if (line.front() == '>') {
            if (auto result = flush(); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
            const std::string_view header(line.data() + 1U, line.size() - 1U);
            if (header.empty()) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "FASTA header is empty"));
            }
            active = true;
            if (header.starts_with(kCanonicalPrefix)) {
                const auto name_marker = header.find("|name_hex=", kCanonicalPrefix.size());
                if (name_marker == std::string_view::npos ||
                    header.find('|', name_marker + 1U) != std::string_view::npos) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "FASTA canonical header is malformed"));
                }
                const auto id_text = header.substr(
                    kCanonicalPrefix.size(), name_marker - kCanonicalPrefix.size());
                std::uint64_t parsed_id = 0U;
                const auto parsed = std::from_chars(
                    id_text.data(), id_text.data() + id_text.size(), parsed_id);
                if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() ||
                    parsed_id == 0U ||
                    genome.contigs().contains(ContigId{parsed_id})) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "FASTA canonical contig id is invalid or duplicated"));
                }
                const auto decoded_name = decode_fasta_name(
                    header.substr(name_marker + std::string_view("|name_hex=").size()));
                if (!decoded_name) {
                    return core::Result<SequenceGenome>::failure(decoded_name.error());
                }
                id = ContigId{parsed_id};
                name = decoded_name.value();
            } else {
                if (auto result = validate_text(header, "FASTA contig header", 256U, true);
                    !result) {
                    return core::Result<SequenceGenome>::failure(result.error());
                }
                while (next_id == 0U || genome.contigs().contains(ContigId{next_id})) {
                    ++next_id;
                }
                if (next_id > kMaxCollectionEntries) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "FASTA contains too many contigs"));
                }
                id = ContigId{next_id++};
                name = std::string(header);
            }
            continue;
        }
        if (!active) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "FASTA sequence bytes precede the first header"));
        }
        if (auto result = validate_sequence(line, "FASTA sequence line"); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (sequence.size() > SequenceGenome::kMaxSequenceBytes - line.size()) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "FASTA aggregate sequence exceeds the safety limit"));
        }
        sequence += line;
    }
    if (input.bad()) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "FASTA input could not be read"));
    }
    if (auto result = flush(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    if (genome.contigs().empty()) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "FASTA contains no contig records"));
    }
    if (auto result = genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    return core::Result<SequenceGenome>::success(std::move(genome));
}

core::Result<SequenceGenomeVcfExport> SequenceGenome::export_vcf() const {
    if (auto result = validate(); !result) {
        return core::Result<SequenceGenomeVcfExport>::failure(
            result.error().with_context("VCF export"));
    }
    if (!source_digest.has_value()) {
        return core::Result<SequenceGenomeVcfExport>::failure(validation(
            "VCF export requires a reference-genome source digest"));
    }
    if (sample_reference.empty()) {
        return core::Result<SequenceGenomeVcfExport>::failure(validation(
            "VCF export requires a sample reference"));
    }
    if (contigs_.empty()) {
        return core::Result<SequenceGenomeVcfExport>::failure(validation(
            "VCF export requires at least one reference contig"));
    }

    std::set<std::string> contig_names;
    const SequenceContig* provenance_contig = nullptr;
    for (const auto& [id, contig] : contigs_) {
        static_cast<void>(id);
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenomeVcfExport>::failure(validation(
                "VCF export cannot represent a digest-only reference contig"));
        }
        if (auto result = validate_vcf_token(contig.name, "VCF contig name", 256U); !result) {
            return core::Result<SequenceGenomeVcfExport>::failure(result.error());
        }
        if (!contig_names.insert(contig.name).second) {
            return core::Result<SequenceGenomeVcfExport>::failure(validation(
                "VCF export requires unique contig names"));
        }
        if (provenance_contig == nullptr && !contig.provenance.source_reference.empty()) {
            provenance_contig = &contig;
        }
    }
    if (provenance_contig == nullptr) {
        return core::Result<SequenceGenomeVcfExport>::failure(validation(
            "VCF export requires a reference provenance source"));
    }

    std::set<std::string> exported_identifiers;
    for (const auto& [id, variant] : variants_) {
        static_cast<void>(id);
        if (auto result = validate_vcf_sequence(variant.reference, "VCF reference allele");
            !result) {
            return core::Result<SequenceGenomeVcfExport>::failure(result.error());
        }
        if (auto result = validate_vcf_sequence(variant.alternate, "VCF alternate allele");
            !result) {
            return core::Result<SequenceGenomeVcfExport>::failure(result.error());
        }
        const std::string identifier = variant.source_identifier.empty()
            ? "cartographer:" + std::to_string(variant.id.value)
            : variant.source_identifier;
        if (identifier == ".") {
            return core::Result<SequenceGenomeVcfExport>::failure(validation(
                "VCF source identifier must not be the missing-value marker"));
        }
        if (!variant.source_identifier.empty() && identifier.starts_with("cartographer:")) {
            return core::Result<SequenceGenomeVcfExport>::failure(validation(
                "VCF source identifiers must not use the reserved cartographer: prefix"));
        }
        if (auto result = validate_vcf_token(identifier, "VCF variant identifier", 256U);
            !result) {
            return core::Result<SequenceGenomeVcfExport>::failure(result.error());
        }
        if (!exported_identifiers.insert(identifier).second) {
            return core::Result<SequenceGenomeVcfExport>::failure(validation(
                "VCF variant identifiers must be unique"));
        }
    }

    constexpr std::size_t kMaxVcfTextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    std::ostringstream output;
    output << "##fileformat=VCFv4.3\n"
           << "##cartographer.reference_digest=" << source_digest->hex() << '\n'
           << "##cartographer.assembly_hex=" << encode_hex_text(assembly) << '\n'
           << "##cartographer.sample_reference_hex=" << encode_hex_text(sample_reference) << '\n'
           << "##cartographer.coordinate_system=vcf-1-based\n"
           << "##cartographer.provenance_source_hex="
           << encode_hex_text(provenance_contig->provenance.source_reference) << '\n'
           << "##cartographer.provenance_release_hex="
           << encode_hex_text(provenance_contig->provenance.release) << '\n'
           << "##cartographer.provenance_license_hex="
           << encode_hex_text(provenance_contig->provenance.license) << '\n'
           << "##cartographer.provenance_provider_hex="
           << encode_hex_text(provenance_contig->provenance.provider) << '\n'
           << "##cartographer.provenance_imported_at_hex="
           << encode_hex_text(provenance_contig->provenance.imported_at_utc) << '\n';
    output << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n";
    for (const auto& [id, variant] : variants_) {
        const auto& contig = contigs_.at(variant.contig);
        output << contig.name << '\t' << (variant.position + 1U) << '\t'
               << (variant.source_identifier.empty()
                       ? "cartographer:" + std::to_string(variant.id.value)
                       : variant.source_identifier)
               << '\t' << variant.reference << '\t' << variant.alternate
               << "\t.\tPASS\tCARTOGRAPHER_KIND_HEX="
               << encode_hex_text(variant.kind) << '\n';
        if (output.tellp() < 0 || static_cast<std::size_t>(output.tellp()) > kMaxVcfTextBytes) {
            return core::Result<SequenceGenomeVcfExport>::failure(validation(
                "VCF export exceeds the safety limit"));
        }
    }

    SequenceGenomeVcfExport result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxVcfTextBytes) {
        return core::Result<SequenceGenomeVcfExport>::failure(validation(
            "VCF export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "VCF carries reference-relative variants but omits full reference contig sequence bytes");
    result.feature_loss_notes.push_back(
        "VCF carries a reference provenance envelope but omits per-contig and per-variant provenance");
    result.feature_loss_notes.push_back(
        "VCF does not carry Cartographer morphology, functional-genome, phenotype, or execution semantics");
    return core::Result<SequenceGenomeVcfExport>::success(std::move(result));
}

core::Result<SequenceGenome> SequenceGenome::import_vcf(
    std::string_view text,
    SequenceGenome reference_genome,
    Provenance provenance) {
    constexpr std::size_t kMaxVcfTextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    if (text.empty() || text.size() > kMaxVcfTextBytes) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "VCF input is empty or exceeds the safety limit"));
    }
    if (auto result = provenance.validate(); !result) {
        return core::Result<SequenceGenome>::failure(result.error().with_context("VCF provenance"));
    }
    if (provenance.source_reference.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "VCF import requires a provenance source reference"));
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("VCF reference genome"));
    }
    if (!reference_genome.source_digest.has_value()) {
        return core::Result<SequenceGenome>::failure(validation(
            "VCF import requires a reference-genome source digest"));
    }
    if (reference_genome.sample_reference.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "VCF import requires a reference-genome sample reference"));
    }
    if (!reference_genome.variants().empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "VCF import requires a reference genome without pre-existing variants"));
    }

    std::map<std::string, ContigId> contigs_by_name;
    for (const auto& [id, contig] : reference_genome.contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF import requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_vcf_token(contig.name, "VCF reference contig name", 256U);
            !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (!contigs_by_name.emplace(contig.name, id).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF import requires unique reference contig names"));
        }
    }

    bool fileformat_seen = false;
    bool column_header_seen = false;
    bool body_seen = false;
    std::optional<std::string> reference_digest_text;
    std::optional<std::string> assembly_hex;
    std::optional<std::string> sample_reference_hex;
    std::optional<std::string> coordinate_system;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    std::vector<SequenceVariant> imported_variants;
    std::set<std::string> source_identifiers;
    std::set<VariantId> imported_ids;

    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };
    const auto metadata_ready = [&]() -> core::Result<void> {
        if (!fileformat_seen || !reference_digest_text.has_value() || !assembly_hex.has_value() ||
            !sample_reference_hex.has_value() || !coordinate_system.has_value() ||
            !provenance_source_hex.has_value() || !provenance_release_hex.has_value() ||
            !provenance_license_hex.has_value() || !provenance_provider_hex.has_value() ||
            !provenance_imported_at_hex.has_value()) {
            return core::Result<void>::failure(parse_error(
                "VCF is missing required Cartographer identity or provenance metadata"));
        }
        if (coordinate_system.value() != "vcf-1-based") {
            return core::Result<void>::failure(validation(
                "VCF coordinate system must be explicitly vcf-1-based"));
        }
        const auto reference_digest = assets::Sha256Digest::from_hex(reference_digest_text.value());
        if (!reference_digest) return core::Result<void>::failure(reference_digest.error());
        if (reference_digest.value() != reference_genome.source_digest.value()) {
            return core::Result<void>::failure(validation(
                "VCF reference digest does not match the supplied reference genome"));
        }
        const auto assembly = decode_hex_text(assembly_hex.value(), "VCF assembly", 256U, true);
        const auto sample = decode_hex_text(
            sample_reference_hex.value(), "VCF sample reference", kMaxReferenceBytes, true);
        const auto source = decode_hex_text(
            provenance_source_hex.value(), "VCF provenance source", kMaxReferenceBytes, true);
        const auto release = decode_hex_text(
            provenance_release_hex.value(), "VCF provenance release", kMaxTextBytes, false);
        const auto license = decode_hex_text(
            provenance_license_hex.value(), "VCF provenance license", kMaxTextBytes, false);
        const auto provider = decode_hex_text(
            provenance_provider_hex.value(), "VCF provenance provider", kMaxTextBytes, false);
        const auto imported_at = decode_hex_text(
            provenance_imported_at_hex.value(), "VCF provenance imported_at", 128U, false);
        if (!assembly || !sample || !source || !release || !license || !provider || !imported_at) {
            return core::Result<void>::failure(parse_error(
                "VCF provenance or identity metadata is malformed"));
        }
        if (assembly.value() != reference_genome.assembly ||
            sample.value() != reference_genome.sample_reference) {
            return core::Result<void>::failure(validation(
                "VCF assembly or sample identity does not match the supplied reference genome"));
        }
        return core::Result<void>::success();
    };

    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.size() > 4U * 1024U * 1024U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "VCF contains an empty or oversized line"));
        }
        if (line_number == 1U && line != "##fileformat=VCFv4.3") {
            return core::Result<SequenceGenome>::failure(parse_error(
                "VCF must begin with fileformat VCFv4.3"));
        }
        if (line.starts_with("##")) {
            if (column_header_seen || body_seen) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "VCF metadata appears after the column header"));
            }
            if (line == "##fileformat=VCFv4.3") {
                if (fileformat_seen) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "VCF fileformat metadata is duplicated"));
                }
                fileformat_seen = true;
                continue;
            }
            constexpr std::string_view kMetadataPrefix = "##cartographer.";
            if (!line.starts_with(kMetadataPrefix)) {
                return core::Result<SequenceGenome>::failure(validation(
                    "VCF contains unsupported metadata that the bounded adapter would discard"));
            }
            const auto separator = line.find('=', kMetadataPrefix.size());
            if (separator == std::string::npos) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "VCF Cartographer metadata is missing its value separator"));
            }
            const auto key = std::string_view(line).substr(
                kMetadataPrefix.size(), separator - kMetadataPrefix.size());
            const auto value = std::string_view(line).substr(separator + 1U);
            core::Result<void> result = core::Result<void>::failure(parse_error(
                "unknown Cartographer VCF metadata key: " + std::string(key)));
            if (key == "reference_digest") result = set_metadata(reference_digest_text, value, key);
            else if (key == "assembly_hex") result = set_metadata(assembly_hex, value, key);
            else if (key == "sample_reference_hex") result = set_metadata(sample_reference_hex, value, key);
            else if (key == "coordinate_system") result = set_metadata(coordinate_system, value, key);
            else if (key == "provenance_source_hex") result = set_metadata(provenance_source_hex, value, key);
            else if (key == "provenance_release_hex") result = set_metadata(provenance_release_hex, value, key);
            else if (key == "provenance_license_hex") result = set_metadata(provenance_license_hex, value, key);
            else if (key == "provenance_provider_hex") result = set_metadata(provenance_provider_hex, value, key);
            else if (key == "provenance_imported_at_hex") result = set_metadata(provenance_imported_at_hex, value, key);
            if (!result) return core::Result<SequenceGenome>::failure(result.error());
            continue;
        }
        if (line == "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO") {
            if (!fileformat_seen || column_header_seen) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "VCF column header is duplicated or follows incomplete metadata"));
            }
            if (auto result = metadata_ready(); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
            column_header_seen = true;
            continue;
        }
        if (!column_header_seen || line.front() == '#') {
            return core::Result<SequenceGenome>::failure(parse_error(
                "VCF contains an unexpected header or record line"));
        }
        body_seen = true;
        const auto fields = split_vcf_fields(line);
        if (fields.size() != 8U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "VCF records must contain exactly eight columns"));
        }
        const auto contig = contigs_by_name.find(std::string(fields[0]));
        if (contig == contigs_by_name.end()) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF record references a contig absent from the supplied reference genome"));
        }
        std::uint64_t position_one_based = 0U;
        const auto parsed_position = std::from_chars(
            fields[1].data(), fields[1].data() + fields[1].size(), position_one_based);
        if (parsed_position.ec != std::errc{} ||
            parsed_position.ptr != fields[1].data() + fields[1].size() ||
            position_one_based == 0U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "VCF position must be a positive decimal 1-based coordinate"));
        }
        if (auto result = validate_vcf_sequence(fields[3], "VCF reference allele"); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (auto result = validate_vcf_sequence(fields[4], "VCF alternate allele"); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (fields[5] != "." || (fields[6] != "." && fields[6] != "PASS")) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF QUAL and FILTER must be '.' or PASS as supported by Cartographer"));
        }
        std::string kind = "vcf";
        if (fields[7] != ".") {
            constexpr std::string_view kKindPrefix = "CARTOGRAPHER_KIND_HEX=";
            if (!fields[7].starts_with(kKindPrefix) ||
                fields[7].find(';') != std::string_view::npos) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "VCF INFO contains unsupported fields"));
            }
            const auto decoded_kind = decode_hex_text(
                fields[7].substr(kKindPrefix.size()), "VCF variant kind", 128U, true);
            if (!decoded_kind) return core::Result<SequenceGenome>::failure(decoded_kind.error());
            kind = decoded_kind.value();
        }

        const auto& reference_contig = reference_genome.contigs_.at(contig->second);
        if (position_one_based > reference_contig.sequence.size() ||
            fields[3].size() > reference_contig.sequence.size() - position_one_based + 1U ||
            reference_contig.sequence.compare(
                static_cast<std::size_t>(position_one_based - 1U), fields[3].size(), fields[3]) != 0) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF reference allele does not match the supplied reference genome"));
        }

        std::string source_identifier;
        VariantId variant_id{};
        if (fields[2] == ".") {
            variant_id = VariantId{stable_variant_id(variant_identity_key(
                fields[0], position_one_based, fields[3], fields[4], ""))};
        } else {
            if (auto result = validate_vcf_token(fields[2], "VCF variant identifier", 256U);
                !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
            if (fields[2].starts_with("cartographer:")) {
                const auto numeric = fields[2].substr(std::string_view("cartographer:").size());
                std::uint64_t parsed_id = 0U;
                const auto parsed = std::from_chars(
                    numeric.data(), numeric.data() + numeric.size(), parsed_id);
                if (parsed.ec != std::errc{} || parsed.ptr != numeric.data() + numeric.size() ||
                    parsed_id == 0U) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "VCF Cartographer variant identifier is invalid"));
                }
                variant_id = VariantId{parsed_id};
            } else {
                source_identifier = std::string(fields[2]);
                if (!source_identifiers.insert(source_identifier).second) {
                    return core::Result<SequenceGenome>::failure(validation(
                        "VCF source identifiers must be unique"));
                }
                variant_id = VariantId{stable_variant_id(variant_identity_key(
                    fields[0], position_one_based, fields[3], fields[4], source_identifier))};
            }
        }
        if (!imported_ids.insert(variant_id).second ||
            reference_genome.variants_.contains(variant_id)) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF variant identifiers collide after canonicalization"));
        }
        if (imported_variants.size() >= kMaxCollectionEntries) {
            return core::Result<SequenceGenome>::failure(validation(
                "VCF variant collection exceeds the safety limit"));
        }
        imported_variants.push_back(SequenceVariant{
            variant_id, contig->second, position_one_based - 1U,
            std::string(fields[3]), std::string(fields[4]), std::move(kind), provenance,
            std::move(source_identifier)});
    }
    if (input.bad() || !column_header_seen) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "VCF is missing its required column header"));
    }
    if (auto result = metadata_ready(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    for (auto& variant : imported_variants) {
        if (auto result = reference_genome.insert_variant(std::move(variant)); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("VCF imported sequence genome"));
    }
    return core::Result<SequenceGenome>::success(std::move(reference_genome));
}

core::Result<SequenceGenomeGff3Export> SequenceGenome::export_gff3() const {
    if (auto result = validate(); !result) {
        return core::Result<SequenceGenomeGff3Export>::failure(
            result.error().with_context("GFF3 export"));
    }
    if (features_.empty()) {
        return core::Result<SequenceGenomeGff3Export>::failure(validation(
            "GFF3 export requires at least one sequence feature"));
    }
    if (!source_digest.has_value()) {
        return core::Result<SequenceGenomeGff3Export>::failure(validation(
            "GFF3 export requires a source genome digest"));
    }

    const SequenceFeature* provenance_feature = nullptr;
    std::map<ContigId, std::string> contig_names;
    std::set<std::string> contig_name_set;
    for (const auto& [id, contig] : contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenomeGff3Export>::failure(validation(
                "GFF3 export requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_gff_token(contig.name, "GFF3 contig name", 256U); !result) {
            return core::Result<SequenceGenomeGff3Export>::failure(result.error());
        }
        if (!contig_name_set.insert(contig.name).second) {
            return core::Result<SequenceGenomeGff3Export>::failure(validation(
                "GFF3 export requires unique contig names"));
        }
        contig_names.emplace(id, contig.name);
    }
    for (const auto& [id, feature] : features_) {
        static_cast<void>(id);
        if (provenance_feature == nullptr && !feature.provenance.source_reference.empty()) {
            provenance_feature = &feature;
        }
        if (auto result = validate_gff_token(feature.type, "GFF3 feature type", 256U); !result) {
            return core::Result<SequenceGenomeGff3Export>::failure(result.error());
        }
        if (!feature.source.empty()) {
            if (auto result = validate_gff_token(feature.source, "GFF3 feature source", 256U);
                !result) {
                return core::Result<SequenceGenomeGff3Export>::failure(result.error());
            }
        }
        for (const auto& attribute : feature.attributes) {
            if (auto result = validate_gff_token(
                    attribute.key, "GFF3 feature attribute key", 128U); !result) {
                return core::Result<SequenceGenomeGff3Export>::failure(result.error());
            }
        }
    }
    if (provenance_feature == nullptr) {
        return core::Result<SequenceGenomeGff3Export>::failure(validation(
            "GFF3 export requires a feature provenance source"));
    }

    constexpr std::size_t kMaxGff3TextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    std::ostringstream output;
    output << "##gff-version 3\n"
           << "##cartographer.reference_digest=" << source_digest->hex() << '\n'
           << "##cartographer.assembly_hex=" << encode_hex_text(assembly) << '\n'
           << "##cartographer.sample_reference_hex=" << encode_hex_text(sample_reference) << '\n'
           << "##cartographer.coordinate_system=gff3-1-based-inclusive\n"
           << "##cartographer.feature_id_attribute=CARTOGRAPHER_ID\n"
           << "##cartographer.provenance_source_hex="
           << encode_hex_text(provenance_feature->provenance.source_reference) << '\n'
           << "##cartographer.provenance_release_hex="
           << encode_hex_text(provenance_feature->provenance.release) << '\n'
           << "##cartographer.provenance_license_hex="
           << encode_hex_text(provenance_feature->provenance.license) << '\n'
           << "##cartographer.provenance_provider_hex="
           << encode_hex_text(provenance_feature->provenance.provider) << '\n'
           << "##cartographer.provenance_imported_at_hex="
           << encode_hex_text(provenance_feature->provenance.imported_at_utc) << '\n';
    output << std::setprecision(17);
    for (const auto& [id, feature] : features_) {
        const auto contig = contig_names.find(feature.contig);
        if (contig == contig_names.end()) {
            return core::Result<SequenceGenomeGff3Export>::failure(validation(
                "GFF3 feature references an unknown contig"));
        }
        output << contig->second << '\t'
               << (feature.source.empty() ? "." : feature.source) << '\t'
               << feature.type << '\t'
               << (feature.start + 1U) << '\t' << feature.end << '\t';
        if (feature.score.has_value()) output << feature.score.value();
        else output << '.';
        output << '\t' << feature.strand << '\t';
        if (feature.phase.has_value()) output << static_cast<unsigned>(*feature.phase);
        else output << '.';
        output << '\t' << "CARTOGRAPHER_ID=" << id.value;
        if (!feature.source_identifier.empty()) {
            output << ";ID=" << gff3_escape(feature.source_identifier);
        }
        for (const auto& attribute : feature.attributes) {
            output << ';' << attribute.key << '=' << gff3_escape(attribute.value);
        }
        output << '\n';
        if (output.tellp() < 0 ||
            static_cast<std::size_t>(output.tellp()) > kMaxGff3TextBytes) {
            return core::Result<SequenceGenomeGff3Export>::failure(validation(
                "GFF3 export exceeds the safety limit"));
        }
    }
    SequenceGenomeGff3Export result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxGff3TextBytes) {
        return core::Result<SequenceGenomeGff3Export>::failure(validation(
            "GFF3 export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "GFF3 carries annotation intervals and bounded attributes, not sequence bytes");
    result.feature_loss_notes.push_back(
        "GFF3 omits per-feature Cartographer provenance; import uses the caller provenance");
    result.feature_loss_notes.push_back(
        "GFF3 annotations do not become functional-genome genes, developmental programs, or phenotype capabilities");
    return core::Result<SequenceGenomeGff3Export>::success(std::move(result));
}

core::Result<SequenceGenome> SequenceGenome::import_gff3(
    std::string_view text,
    SequenceGenome reference_genome,
    Provenance provenance) {
    constexpr std::size_t kMaxGff3TextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    if (text.size() > kMaxGff3TextBytes) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "GFF3 record is too large"));
    }
    if (auto result = provenance.validate(); !result || provenance.source_reference.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "GFF3 import requires non-empty caller provenance"));
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("GFF3 reference genome"));
    }
    if (!reference_genome.source_digest.has_value()) {
        return core::Result<SequenceGenome>::failure(validation(
            "GFF3 import requires a reference genome source digest"));
    }
    if (!reference_genome.features_.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "GFF3 import requires a reference genome without pre-existing features"));
    }

    std::map<std::string, ContigId> contigs_by_name;
    for (const auto& [id, contig] : reference_genome.contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 import requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_gff_token(contig.name, "GFF3 reference contig name", 256U);
            !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (!contigs_by_name.emplace(contig.name, id).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 import requires unique reference contig names"));
        }
    }

    bool fileformat_seen = false;
    bool body_seen = false;
    std::optional<std::string> reference_digest_text;
    std::optional<std::string> assembly_hex;
    std::optional<std::string> sample_reference_hex;
    std::optional<std::string> coordinate_system;
    std::optional<std::string> feature_id_attribute;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    std::vector<SequenceFeature> imported_features;
    std::set<std::string> source_identifiers;
    std::set<SequenceFeatureId> imported_ids;

    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };
    const auto metadata_ready = [&]() -> core::Result<void> {
        if (!fileformat_seen || !reference_digest_text.has_value() || !assembly_hex.has_value() ||
            !sample_reference_hex.has_value() || !coordinate_system.has_value() ||
            !feature_id_attribute.has_value() || !provenance_source_hex.has_value() ||
            !provenance_release_hex.has_value() || !provenance_license_hex.has_value() ||
            !provenance_provider_hex.has_value() || !provenance_imported_at_hex.has_value()) {
            return core::Result<void>::failure(parse_error(
                "GFF3 is missing required Cartographer identity or provenance metadata"));
        }
        if (coordinate_system.value() != "gff3-1-based-inclusive") {
            return core::Result<void>::failure(validation(
                "GFF3 coordinate system must be explicitly gff3-1-based-inclusive"));
        }
        if (feature_id_attribute.value() != "CARTOGRAPHER_ID") {
            return core::Result<void>::failure(validation(
                "GFF3 feature identity attribute is unsupported"));
        }
        const auto reference_digest = assets::Sha256Digest::from_hex(reference_digest_text.value());
        if (!reference_digest) return core::Result<void>::failure(reference_digest.error());
        if (reference_digest.value() != reference_genome.source_digest.value()) {
            return core::Result<void>::failure(validation(
                "GFF3 reference digest does not match the supplied reference genome"));
        }
        const auto assembly = decode_hex_text(assembly_hex.value(), "GFF3 assembly", 256U, true);
        const auto sample = decode_hex_text(
            sample_reference_hex.value(), "GFF3 sample reference", kMaxReferenceBytes, false);
        const auto source = decode_hex_text(
            provenance_source_hex.value(), "GFF3 provenance source", kMaxReferenceBytes, true);
        const auto release = decode_hex_text(
            provenance_release_hex.value(), "GFF3 provenance release", kMaxTextBytes, false);
        const auto license = decode_hex_text(
            provenance_license_hex.value(), "GFF3 provenance license", kMaxTextBytes, false);
        const auto provider = decode_hex_text(
            provenance_provider_hex.value(), "GFF3 provenance provider", kMaxTextBytes, false);
        const auto imported_at = decode_hex_text(
            provenance_imported_at_hex.value(), "GFF3 provenance imported_at", 128U, false);
        if (!assembly || !sample || !source || !release || !license || !provider || !imported_at) {
            return core::Result<void>::failure(parse_error(
                "GFF3 provenance or identity metadata is malformed"));
        }
        if (assembly.value() != reference_genome.assembly ||
            sample.value() != reference_genome.sample_reference) {
            return core::Result<void>::failure(validation(
                "GFF3 assembly or sample identity does not match the supplied reference genome"));
        }
        return core::Result<void>::success();
    };

    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.size() > 4U * 1024U * 1024U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GFF3 contains an empty or oversized line"));
        }
        if (line_number == 1U && line != "##gff-version 3") {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GFF3 must begin with ##gff-version 3"));
        }
        if (line.starts_with("##")) {
            if (body_seen) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GFF3 metadata appears after a feature record"));
            }
            if (line == "##gff-version 3") {
                if (fileformat_seen) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "GFF3 version metadata is duplicated"));
                }
                fileformat_seen = true;
                continue;
            }
            constexpr std::string_view kMetadataPrefix = "##cartographer.";
            if (!line.starts_with(kMetadataPrefix)) {
                return core::Result<SequenceGenome>::failure(validation(
                    "GFF3 contains unsupported metadata that the bounded adapter would discard"));
            }
            const auto separator = line.find('=', kMetadataPrefix.size());
            if (separator == std::string::npos) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GFF3 Cartographer metadata is missing its value separator"));
            }
            const auto key = std::string_view(line).substr(
                kMetadataPrefix.size(), separator - kMetadataPrefix.size());
            const auto value = std::string_view(line).substr(separator + 1U);
            core::Result<void> result = core::Result<void>::failure(parse_error(
                "unknown Cartographer GFF3 metadata key: " + std::string(key)));
            if (key == "reference_digest") result = set_metadata(reference_digest_text, value, key);
            else if (key == "assembly_hex") result = set_metadata(assembly_hex, value, key);
            else if (key == "sample_reference_hex") result = set_metadata(sample_reference_hex, value, key);
            else if (key == "coordinate_system") result = set_metadata(coordinate_system, value, key);
            else if (key == "feature_id_attribute") result = set_metadata(feature_id_attribute, value, key);
            else if (key == "provenance_source_hex") result = set_metadata(provenance_source_hex, value, key);
            else if (key == "provenance_release_hex") result = set_metadata(provenance_release_hex, value, key);
            else if (key == "provenance_license_hex") result = set_metadata(provenance_license_hex, value, key);
            else if (key == "provenance_provider_hex") result = set_metadata(provenance_provider_hex, value, key);
            else if (key == "provenance_imported_at_hex") result = set_metadata(provenance_imported_at_hex, value, key);
            if (!result) return core::Result<SequenceGenome>::failure(result.error());
            continue;
        }
        if (line.front() == '#') {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 contains an unsupported comment or directive"));
        }
        if (!body_seen) {
            if (auto result = metadata_ready(); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
        }
        body_seen = true;
        const auto fields = split_vcf_fields(line);
        if (fields.size() != 9U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GFF3 records must contain exactly nine columns"));
        }
        const auto contig = contigs_by_name.find(std::string(fields[0]));
        if (contig == contigs_by_name.end()) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 record references a contig absent from the supplied reference genome"));
        }
        if (auto result = validate_gff_token(fields[1] == "." ? "" : fields[1],
                                              "GFF3 feature source", 256U, false); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (fields[2].empty()) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GFF3 feature type must be non-empty"));
        }
        if (auto result = validate_gff_token(fields[2], "GFF3 feature type", 256U); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        std::uint64_t start_one_based = 0U;
        std::uint64_t end_one_based = 0U;
        const auto parsed_start = std::from_chars(
            fields[3].data(), fields[3].data() + fields[3].size(), start_one_based);
        const auto parsed_end = std::from_chars(
            fields[4].data(), fields[4].data() + fields[4].size(), end_one_based);
        if (parsed_start.ec != std::errc{} ||
            parsed_start.ptr != fields[3].data() + fields[3].size() ||
            parsed_end.ec != std::errc{} ||
            parsed_end.ptr != fields[4].data() + fields[4].size() ||
            start_one_based == 0U || end_one_based < start_one_based) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GFF3 coordinates must be positive one-based inclusive integers"));
        }
        const auto& reference_contig = reference_genome.contigs_.at(contig->second);
        if (end_one_based > reference_contig.sequence.size()) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 feature interval lies outside its reference contig"));
        }
        std::optional<double> score;
        if (fields[5] != ".") {
            const auto parsed_score = parse_gff_double(fields[5], "GFF3 score");
            if (!parsed_score) return core::Result<SequenceGenome>::failure(parsed_score.error());
            score = parsed_score.value();
        }
        if (fields[6] != "." && fields[6] != "+" && fields[6] != "-" && fields[6] != "?") {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 strand is unsupported"));
        }
        std::optional<std::uint8_t> phase;
        if (fields[7] != ".") {
            if (fields[7].size() != 1U || fields[7][0] < '0' || fields[7][0] > '2') {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GFF3 phase must be '.', '0', '1', or '2'"));
            }
            phase = static_cast<std::uint8_t>(fields[7][0] - '0');
        }

        std::string source_identifier;
        std::optional<std::uint64_t> explicit_id;
        std::vector<SequenceFeatureAttribute> attributes;
        std::set<std::string> attribute_keys;
        if (fields[8] != ".") {
            std::size_t start = 0U;
            while (start <= fields[8].size()) {
                const auto separator = fields[8].find(';', start);
                const auto part = fields[8].substr(
                    start, separator == std::string_view::npos
                        ? fields[8].size() - start : separator - start);
                if (part.empty()) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "GFF3 attribute list contains an empty entry"));
                }
                const auto equals = part.find('=');
                if (equals == std::string_view::npos || part.find('=', equals + 1U) != std::string_view::npos) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "GFF3 attribute entry must contain exactly one '='"));
                }
                const auto key = part.substr(0U, equals);
                if (auto result = validate_gff_token(key, "GFF3 attribute key", 128U); !result) {
                    return core::Result<SequenceGenome>::failure(result.error());
                }
                if (!attribute_keys.insert(std::string(key)).second) {
                    return core::Result<SequenceGenome>::failure(validation(
                        "GFF3 attribute keys must be unique"));
                }
                const auto decoded = gff3_unescape(
                    part.substr(equals + 1U), "GFF3 attribute value", 16U * 1024U, false);
                if (!decoded) return core::Result<SequenceGenome>::failure(decoded.error());
                if (key == "ID") {
                    if (decoded.value().empty()) {
                        return core::Result<SequenceGenome>::failure(parse_error(
                            "GFF3 ID attribute must be non-empty"));
                    }
                    source_identifier = decoded.value();
                } else if (key == "CARTOGRAPHER_ID") {
                    std::uint64_t value = 0U;
                    const auto parsed = std::from_chars(
                        decoded.value().data(), decoded.value().data() + decoded.value().size(), value);
                    if (parsed.ec != std::errc{} ||
                        parsed.ptr != decoded.value().data() + decoded.value().size() || value == 0U) {
                        return core::Result<SequenceGenome>::failure(parse_error(
                            "GFF3 Cartographer feature id is invalid"));
                    }
                    explicit_id = value;
                } else {
                    attributes.push_back({std::string(key), decoded.value()});
                }
                if (separator == std::string_view::npos) break;
                start = separator + 1U;
            }
        }
        if (!source_identifier.empty() && !source_identifiers.insert(source_identifier).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 source identifiers must be unique"));
        }
        const auto canonical_id = feature_identity_key(
            fields[0], start_one_based - 1U, end_one_based, fields[2], source_identifier, fields[8]);
        const SequenceFeatureId feature_id{
            explicit_id.value_or(stable_feature_id(canonical_id))};
        if (!imported_ids.insert(feature_id).second || reference_genome.features_.contains(feature_id)) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 feature identifiers collide after canonicalization"));
        }
        imported_features.push_back(SequenceFeature{
            feature_id, contig->second, start_one_based - 1U, end_one_based,
            std::string(fields[2]), fields[1] == "." ? std::string{} : std::string(fields[1]),
            score, std::string(fields[6]), phase, std::move(source_identifier),
            std::move(attributes), provenance});
        if (imported_features.size() > kMaxCollectionEntries) {
            return core::Result<SequenceGenome>::failure(validation(
                "GFF3 feature collection exceeds the safety limit"));
        }
    }
    if (input.bad() || !fileformat_seen || !body_seen) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "GFF3 is missing its required version or feature records"));
    }
    if (auto result = metadata_ready(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    for (auto& feature : imported_features) {
        if (auto result = reference_genome.insert_feature(std::move(feature)); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("GFF3 imported sequence genome"));
    }
    return core::Result<SequenceGenome>::success(std::move(reference_genome));
}

core::Result<SequenceGenomeBed6Export> SequenceGenome::export_bed6() const {
    if (auto result = validate(); !result) {
        return core::Result<SequenceGenomeBed6Export>::failure(
            result.error().with_context("BED6 export"));
    }
    if (features_.empty()) {
        return core::Result<SequenceGenomeBed6Export>::failure(validation(
            "BED6 export requires at least one sequence feature"));
    }
    if (!source_digest.has_value()) {
        return core::Result<SequenceGenomeBed6Export>::failure(validation(
            "BED6 export requires a source genome digest"));
    }

    const SequenceFeature* provenance_feature = nullptr;
    std::map<ContigId, std::string> contig_names;
    std::set<std::string> contig_name_set;
    for (const auto& [id, contig] : contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenomeBed6Export>::failure(validation(
                "BED6 export requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_bed_token(contig.name, "BED6 contig name", 256U); !result) {
            return core::Result<SequenceGenomeBed6Export>::failure(result.error());
        }
        if (!contig_name_set.insert(contig.name).second) {
            return core::Result<SequenceGenomeBed6Export>::failure(validation(
                "BED6 export requires unique contig names"));
        }
        contig_names.emplace(id, contig.name);
    }
    for (const auto& [id, feature] : features_) {
        static_cast<void>(id);
        if (provenance_feature == nullptr && !feature.provenance.source_reference.empty()) {
            provenance_feature = &feature;
        }
        if (feature.strand == "?") {
            return core::Result<SequenceGenomeBed6Export>::failure(validation(
                "BED6 cannot represent the unknown '?' strand"));
        }
    }
    if (provenance_feature == nullptr) {
        return core::Result<SequenceGenomeBed6Export>::failure(validation(
            "BED6 export requires a feature provenance source"));
    }

    constexpr std::size_t kMaxBed6TextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    std::ostringstream output;
    output << "#cartographer.bed_version=6\n"
           << "#cartographer.reference_digest=" << source_digest->hex() << '\n'
           << "#cartographer.assembly_hex=" << encode_hex_text(assembly) << '\n'
           << "#cartographer.sample_reference_hex=" << encode_hex_text(sample_reference) << '\n'
           << "#cartographer.coordinate_system=bed-0-based-half-open\n"
           << "#cartographer.feature_identity=cartographer-name-v1\n"
           << "#cartographer.provenance_source_hex="
           << encode_hex_text(provenance_feature->provenance.source_reference) << '\n'
           << "#cartographer.provenance_release_hex="
           << encode_hex_text(provenance_feature->provenance.release) << '\n'
           << "#cartographer.provenance_license_hex="
           << encode_hex_text(provenance_feature->provenance.license) << '\n'
           << "#cartographer.provenance_provider_hex="
           << encode_hex_text(provenance_feature->provenance.provider) << '\n'
           << "#cartographer.provenance_imported_at_hex="
           << encode_hex_text(provenance_feature->provenance.imported_at_utc) << '\n';
    for (const auto& [id, feature] : features_) {
        const auto contig = contig_names.find(feature.contig);
        if (contig == contig_names.end()) {
            return core::Result<SequenceGenomeBed6Export>::failure(validation(
                "BED6 feature references an unknown contig"));
        }
        std::uint64_t score = 0U;
        if (feature.score.has_value()) {
            if (*feature.score < 0.0 || *feature.score > 1000.0 ||
                !std::isfinite(*feature.score)) {
                return core::Result<SequenceGenomeBed6Export>::failure(validation(
                    "BED6 feature score must be finite and within 0..1000"));
            }
            score = static_cast<std::uint64_t>(std::llround(*feature.score));
            if (score > 1000U) {
                return core::Result<SequenceGenomeBed6Export>::failure(validation(
                    "BED6 feature score rounding exceeds 1000"));
            }
        }
        const std::string name =
            "cartographer:" + std::to_string(id.value) + ":" +
            encode_hex_text(feature.source_identifier);
        if (auto result = validate_bed_token(name, "BED6 feature name", 1024U); !result) {
            return core::Result<SequenceGenomeBed6Export>::failure(result.error());
        }
        output << contig->second << '\t' << feature.start << '\t' << feature.end << '\t'
               << name << '\t' << score << '\t' << feature.strand << '\n';
        if (output.tellp() < 0 ||
            static_cast<std::size_t>(output.tellp()) > kMaxBed6TextBytes) {
            return core::Result<SequenceGenomeBed6Export>::failure(validation(
                "BED6 export exceeds the safety limit"));
        }
    }
    SequenceGenomeBed6Export result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxBed6TextBytes) {
        return core::Result<SequenceGenomeBed6Export>::failure(validation(
            "BED6 export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "BED6 preserves interval, stable identity, bounded name, integer score, and strand only");
    result.feature_loss_notes.push_back(
        "BED6 omits sequence feature type, source, phase, and attributes; score is rounded to its 0..1000 integer scale");
    result.feature_loss_notes.push_back(
        "BED6 omits per-feature Cartographer provenance; import uses the caller provenance");
    result.feature_loss_notes.push_back(
        "BED6 annotations do not become functional-genome genes, developmental programs, or phenotype capabilities");
    return core::Result<SequenceGenomeBed6Export>::success(std::move(result));
}

core::Result<SequenceGenome> SequenceGenome::import_bed6(
    std::string_view text,
    SequenceGenome reference_genome,
    Provenance provenance) {
    constexpr std::size_t kMaxBed6TextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    if (text.size() > kMaxBed6TextBytes) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "BED6 record is too large"));
    }
    if (auto result = provenance.validate(); !result || provenance.source_reference.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "BED6 import requires non-empty caller provenance"));
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("BED6 reference genome"));
    }
    if (!reference_genome.source_digest.has_value()) {
        return core::Result<SequenceGenome>::failure(validation(
            "BED6 import requires a reference genome source digest"));
    }
    if (!reference_genome.features_.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "BED6 import requires a reference genome without pre-existing features"));
    }

    std::map<std::string, ContigId> contigs_by_name;
    for (const auto& [id, contig] : reference_genome.contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 import requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_bed_token(contig.name, "BED6 reference contig name", 256U);
            !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (!contigs_by_name.emplace(contig.name, id).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 import requires unique reference contig names"));
        }
    }

    bool version_seen = false;
    bool body_seen = false;
    std::optional<std::string> reference_digest_text;
    std::optional<std::string> assembly_hex;
    std::optional<std::string> sample_reference_hex;
    std::optional<std::string> coordinate_system;
    std::optional<std::string> feature_identity;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    std::vector<SequenceFeature> imported_features;
    std::set<std::string> source_identifiers;
    std::set<SequenceFeatureId> imported_ids;

    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };
    const auto metadata_ready = [&]() -> core::Result<void> {
        if (!version_seen || !reference_digest_text.has_value() || !assembly_hex.has_value() ||
            !sample_reference_hex.has_value() || !coordinate_system.has_value() ||
            !feature_identity.has_value() || !provenance_source_hex.has_value() ||
            !provenance_release_hex.has_value() || !provenance_license_hex.has_value() ||
            !provenance_provider_hex.has_value() || !provenance_imported_at_hex.has_value()) {
            return core::Result<void>::failure(parse_error(
                "BED6 is missing required Cartographer identity or provenance metadata"));
        }
        if (coordinate_system.value() != "bed-0-based-half-open") {
            return core::Result<void>::failure(validation(
                "BED6 coordinate system must be explicitly bed-0-based-half-open"));
        }
        if (feature_identity.value() != "cartographer-name-v1") {
            return core::Result<void>::failure(validation(
                "BED6 feature identity metadata is unsupported"));
        }
        const auto reference_digest = assets::Sha256Digest::from_hex(reference_digest_text.value());
        if (!reference_digest) return core::Result<void>::failure(reference_digest.error());
        if (reference_digest.value() != reference_genome.source_digest.value()) {
            return core::Result<void>::failure(validation(
                "BED6 reference digest does not match the supplied reference genome"));
        }
        const auto assembly = decode_hex_text(assembly_hex.value(), "BED6 assembly", 256U, true);
        const auto sample = decode_hex_text(
            sample_reference_hex.value(), "BED6 sample reference", kMaxReferenceBytes, false);
        const auto source = decode_hex_text(
            provenance_source_hex.value(), "BED6 provenance source", kMaxReferenceBytes, true);
        const auto release = decode_hex_text(
            provenance_release_hex.value(), "BED6 provenance release", kMaxTextBytes, false);
        const auto license = decode_hex_text(
            provenance_license_hex.value(), "BED6 provenance license", kMaxTextBytes, false);
        const auto provider = decode_hex_text(
            provenance_provider_hex.value(), "BED6 provenance provider", kMaxTextBytes, false);
        const auto imported_at = decode_hex_text(
            provenance_imported_at_hex.value(), "BED6 provenance imported_at", 128U, false);
        if (!assembly || !sample || !source || !release || !license || !provider || !imported_at) {
            return core::Result<void>::failure(parse_error(
                "BED6 provenance or identity metadata is malformed"));
        }
        if (assembly.value() != reference_genome.assembly ||
            sample.value() != reference_genome.sample_reference) {
            return core::Result<void>::failure(validation(
                "BED6 assembly or sample identity does not match the supplied reference genome"));
        }
        return core::Result<void>::success();
    };

    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.size() > 4U * 1024U * 1024U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "BED6 contains an empty or oversized line"));
        }
        if (line_number == 1U && line != "#cartographer.bed_version=6") {
            return core::Result<SequenceGenome>::failure(parse_error(
                "BED6 must begin with #cartographer.bed_version=6"));
        }
        if (line.starts_with("#")) {
            if (body_seen) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "BED6 metadata appears after a feature record"));
            }
            constexpr std::string_view kMetadataPrefix = "#cartographer.";
            if (!line.starts_with(kMetadataPrefix)) {
                return core::Result<SequenceGenome>::failure(validation(
                    "BED6 contains unsupported metadata that the bounded adapter would discard"));
            }
            const auto separator = line.find('=', kMetadataPrefix.size());
            if (separator == std::string::npos) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "BED6 Cartographer metadata is missing its value separator"));
            }
            const auto key = std::string_view(line).substr(
                kMetadataPrefix.size(), separator - kMetadataPrefix.size());
            const auto value = std::string_view(line).substr(separator + 1U);
            core::Result<void> result = core::Result<void>::failure(parse_error(
                "unknown Cartographer BED6 metadata key: " + std::string(key)));
            if (key == "bed_version") {
                if (version_seen) {
                    result = core::Result<void>::failure(parse_error(
                        "bed_version is duplicated"));
                } else if (value != "6") {
                    result = core::Result<void>::failure(validation(
                        "BED6 version metadata must be 6"));
                } else {
                    version_seen = true;
                    result = core::Result<void>::success();
                }
            } else if (key == "reference_digest") result = set_metadata(reference_digest_text, value, key);
            else if (key == "assembly_hex") result = set_metadata(assembly_hex, value, key);
            else if (key == "sample_reference_hex") result = set_metadata(sample_reference_hex, value, key);
            else if (key == "coordinate_system") result = set_metadata(coordinate_system, value, key);
            else if (key == "feature_identity") result = set_metadata(feature_identity, value, key);
            else if (key == "provenance_source_hex") result = set_metadata(provenance_source_hex, value, key);
            else if (key == "provenance_release_hex") result = set_metadata(provenance_release_hex, value, key);
            else if (key == "provenance_license_hex") result = set_metadata(provenance_license_hex, value, key);
            else if (key == "provenance_provider_hex") result = set_metadata(provenance_provider_hex, value, key);
            else if (key == "provenance_imported_at_hex") result = set_metadata(provenance_imported_at_hex, value, key);
            if (!result) return core::Result<SequenceGenome>::failure(result.error());
            continue;
        }
        if (!body_seen) {
            if (auto result = metadata_ready(); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
        }
        body_seen = true;
        const auto fields = split_vcf_fields(line);
        if (fields.size() != 6U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "BED6 records must contain exactly six columns"));
        }
        const auto contig = contigs_by_name.find(std::string(fields[0]));
        if (contig == contigs_by_name.end()) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 record references a contig absent from the supplied reference genome"));
        }
        std::uint64_t start = 0U;
        std::uint64_t end = 0U;
        const auto parsed_start = std::from_chars(
            fields[1].data(), fields[1].data() + fields[1].size(), start);
        const auto parsed_end = std::from_chars(
            fields[2].data(), fields[2].data() + fields[2].size(), end);
        if (parsed_start.ec != std::errc{} || parsed_start.ptr != fields[1].data() + fields[1].size() ||
            parsed_end.ec != std::errc{} || parsed_end.ptr != fields[2].data() + fields[2].size() ||
            start >= end) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "BED6 coordinates must be non-empty zero-based half-open integers"));
        }
        const auto& reference_contig = reference_genome.contigs_.at(contig->second);
        if (end > reference_contig.sequence.size()) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 feature interval lies outside its reference contig"));
        }
        if (auto result = validate_bed_token(fields[3], "BED6 feature name", 1024U); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        std::uint64_t score = 0U;
        const auto parsed_score = std::from_chars(
            fields[4].data(), fields[4].data() + fields[4].size(), score);
        if (parsed_score.ec != std::errc{} || parsed_score.ptr != fields[4].data() + fields[4].size() ||
            score > 1000U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "BED6 score must be a decimal integer from zero through 1000"));
        }
        if (fields[5] != "." && fields[5] != "+" && fields[5] != "-") {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 strand must be one of ., +, or -"));
        }

        SequenceFeatureId feature_id{};
        std::string source_identifier;
        constexpr std::string_view kCartographerPrefix = "cartographer:";
        if (fields[3].starts_with(kCartographerPrefix)) {
            const auto separator = fields[3].find(':', kCartographerPrefix.size());
            if (separator == std::string_view::npos) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "BED6 Cartographer feature name is missing its source-id separator"));
            }
            const auto id_text = fields[3].substr(
                kCartographerPrefix.size(), separator - kCartographerPrefix.size());
            std::uint64_t id = 0U;
            const auto parsed_id = std::from_chars(
                id_text.data(), id_text.data() + id_text.size(), id);
            if (parsed_id.ec != std::errc{} || parsed_id.ptr != id_text.data() + id_text.size() || id == 0U) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "BED6 Cartographer feature id is invalid"));
            }
            const auto decoded = decode_hex_text(
                fields[3].substr(separator + 1U), "BED6 source identifier", 256U, false);
            if (!decoded) return core::Result<SequenceGenome>::failure(decoded.error());
            source_identifier = decoded.value();
            feature_id = SequenceFeatureId{id};
        } else {
            source_identifier = std::string(fields[3]);
            feature_id = SequenceFeatureId{stable_feature_id(feature_identity_key(
                fields[0], start, end, "bed6", source_identifier, fields[3]))};
        }
        if (!source_identifier.empty() && !source_identifiers.insert(source_identifier).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 source identifiers must be unique"));
        }
        if (!imported_ids.insert(feature_id).second || reference_genome.features_.contains(feature_id)) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 feature identifiers collide after canonicalization"));
        }
        imported_features.push_back(SequenceFeature{
            feature_id, contig->second, start, end, "bed6", {},
            static_cast<double>(score), std::string(fields[5]), std::nullopt,
            std::move(source_identifier), {}, provenance});
        if (imported_features.size() > kMaxCollectionEntries) {
            return core::Result<SequenceGenome>::failure(validation(
                "BED6 feature collection exceeds the safety limit"));
        }
    }
    if (input.bad() || !version_seen || !body_seen) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "BED6 is missing its required version or feature records"));
    }
    if (auto result = metadata_ready(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    for (auto& feature : imported_features) {
        if (auto result = reference_genome.insert_feature(std::move(feature)); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("BED6 imported sequence genome"));
    }
    return core::Result<SequenceGenome>::success(std::move(reference_genome));
}

core::Result<SequenceGenomeGtfExport> SequenceGenome::export_gtf() const {
    if (auto result = validate(); !result) {
        return core::Result<SequenceGenomeGtfExport>::failure(
            result.error().with_context("GTF export"));
    }
    if (features_.empty()) {
        return core::Result<SequenceGenomeGtfExport>::failure(validation(
            "GTF export requires at least one sequence feature"));
    }
    if (!source_digest.has_value()) {
        return core::Result<SequenceGenomeGtfExport>::failure(validation(
            "GTF export requires a source genome digest"));
    }

    const SequenceFeature* provenance_feature = nullptr;
    std::map<ContigId, std::string> contig_names;
    std::set<std::string> contig_name_set;
    for (const auto& [id, contig] : contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenomeGtfExport>::failure(validation(
                "GTF export requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_gff_token(contig.name, "GTF contig name", 256U); !result) {
            return core::Result<SequenceGenomeGtfExport>::failure(result.error());
        }
        if (!contig_name_set.insert(contig.name).second) {
            return core::Result<SequenceGenomeGtfExport>::failure(validation(
                "GTF export requires unique contig names"));
        }
        contig_names.emplace(id, contig.name);
    }
    for (const auto& [id, feature] : features_) {
        static_cast<void>(id);
        if (provenance_feature == nullptr && !feature.provenance.source_reference.empty()) {
            provenance_feature = &feature;
        }
        if (auto result = validate_gff_token(feature.type, "GTF feature type", 256U); !result) {
            return core::Result<SequenceGenomeGtfExport>::failure(result.error());
        }
        if (!feature.source.empty()) {
            if (auto result = validate_gff_token(feature.source, "GTF feature source", 256U);
                !result) {
                return core::Result<SequenceGenomeGtfExport>::failure(result.error());
            }
        }
        for (const auto& attribute : feature.attributes) {
            if (auto result = validate_gff_token(
                    attribute.key, "GTF attribute key", 128U); !result) {
                return core::Result<SequenceGenomeGtfExport>::failure(result.error());
            }
        }
    }
    if (provenance_feature == nullptr) {
        return core::Result<SequenceGenomeGtfExport>::failure(validation(
            "GTF export requires a feature provenance source"));
    }

    constexpr std::size_t kMaxGtfTextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    std::ostringstream output;
    output << "#!cartographer.gtf_version=2.2\n"
           << "#!cartographer.reference_digest=" << source_digest->hex() << '\n'
           << "#!cartographer.assembly_hex=" << encode_hex_text(assembly) << '\n'
           << "#!cartographer.sample_reference_hex=" << encode_hex_text(sample_reference) << '\n'
           << "#!cartographer.coordinate_system=gtf-1-based-inclusive\n"
           << "#!cartographer.feature_identity=CARTOGRAPHER_ID\n"
           << "#!cartographer.provenance_source_hex="
           << encode_hex_text(provenance_feature->provenance.source_reference) << '\n'
           << "#!cartographer.provenance_release_hex="
           << encode_hex_text(provenance_feature->provenance.release) << '\n'
           << "#!cartographer.provenance_license_hex="
           << encode_hex_text(provenance_feature->provenance.license) << '\n'
           << "#!cartographer.provenance_provider_hex="
           << encode_hex_text(provenance_feature->provenance.provider) << '\n'
           << "#!cartographer.provenance_imported_at_hex="
           << encode_hex_text(provenance_feature->provenance.imported_at_utc) << '\n';
    output << std::setprecision(17);
    for (const auto& [id, feature] : features_) {
        const auto contig = contig_names.find(feature.contig);
        if (contig == contig_names.end()) {
            return core::Result<SequenceGenomeGtfExport>::failure(validation(
                "GTF feature references an unknown contig"));
        }
        output << contig->second << '\t'
               << (feature.source.empty() ? "." : feature.source) << '\t'
               << feature.type << '\t' << (feature.start + 1U) << '\t' << feature.end << '\t';
        if (feature.score.has_value()) output << feature.score.value();
        else output << '.';
        output << '\t' << feature.strand << '\t';
        if (feature.phase.has_value()) output << static_cast<unsigned>(*feature.phase);
        else output << '.';
        output << "\tCARTOGRAPHER_ID \"" << id.value << "\";";
        if (!feature.source_identifier.empty()) {
            output << " cartographer_source_id \""
                   << gtf_escape(feature.source_identifier) << "\";";
        }
        for (const auto& attribute : feature.attributes) {
            output << ' ' << attribute.key << " \"" << gtf_escape(attribute.value) << "\";";
        }
        output << '\n';
        if (output.tellp() < 0 ||
            static_cast<std::size_t>(output.tellp()) > kMaxGtfTextBytes) {
            return core::Result<SequenceGenomeGtfExport>::failure(validation(
                "GTF export exceeds the safety limit"));
        }
    }
    SequenceGenomeGtfExport result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxGtfTextBytes) {
        return core::Result<SequenceGenomeGtfExport>::failure(validation(
            "GTF export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "GTF carries annotation intervals, bounded attributes, and the supported nine-column fields, not sequence bytes");
    result.feature_loss_notes.push_back(
        "GTF omits per-feature Cartographer provenance; import uses the caller provenance");
    result.feature_loss_notes.push_back(
        "GTF annotations do not become functional-genome genes, developmental programs, or phenotype capabilities");
    return core::Result<SequenceGenomeGtfExport>::success(std::move(result));
}

core::Result<SequenceGenome> SequenceGenome::import_gtf(
    std::string_view text,
    SequenceGenome reference_genome,
    Provenance provenance) {
    constexpr std::size_t kMaxGtfTextBytes =
        SequenceGenome::kMaxSequenceBytes + 4U * 1024U * 1024U;
    if (text.size() > kMaxGtfTextBytes) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "GTF record is too large"));
    }
    if (auto result = provenance.validate(); !result || provenance.source_reference.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "GTF import requires non-empty caller provenance"));
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("GTF reference genome"));
    }
    if (!reference_genome.source_digest.has_value()) {
        return core::Result<SequenceGenome>::failure(validation(
            "GTF import requires a reference genome source digest"));
    }
    if (!reference_genome.features_.empty()) {
        return core::Result<SequenceGenome>::failure(validation(
            "GTF import requires a reference genome without pre-existing features"));
    }

    std::map<std::string, ContigId> contigs_by_name;
    for (const auto& [id, contig] : reference_genome.contigs_) {
        if (contig.sequence.empty()) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF import requires sequence bytes for every reference contig"));
        }
        if (auto result = validate_gff_token(contig.name, "GTF reference contig name", 256U);
            !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (!contigs_by_name.emplace(contig.name, id).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF import requires unique reference contig names"));
        }
    }

    bool version_seen = false;
    bool body_seen = false;
    std::optional<std::string> reference_digest_text;
    std::optional<std::string> assembly_hex;
    std::optional<std::string> sample_reference_hex;
    std::optional<std::string> coordinate_system;
    std::optional<std::string> feature_identity;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    std::vector<SequenceFeature> imported_features;
    std::set<std::string> source_identifiers;
    std::set<SequenceFeatureId> imported_ids;

    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };
    const auto metadata_ready = [&]() -> core::Result<void> {
        if (!version_seen || !reference_digest_text.has_value() || !assembly_hex.has_value() ||
            !sample_reference_hex.has_value() || !coordinate_system.has_value() ||
            !feature_identity.has_value() || !provenance_source_hex.has_value() ||
            !provenance_release_hex.has_value() || !provenance_license_hex.has_value() ||
            !provenance_provider_hex.has_value() || !provenance_imported_at_hex.has_value()) {
            return core::Result<void>::failure(parse_error(
                "GTF is missing required Cartographer identity or provenance metadata"));
        }
        if (coordinate_system.value() != "gtf-1-based-inclusive") {
            return core::Result<void>::failure(validation(
                "GTF coordinate system must be explicitly gtf-1-based-inclusive"));
        }
        if (feature_identity.value() != "CARTOGRAPHER_ID") {
            return core::Result<void>::failure(validation(
                "GTF feature identity metadata is unsupported"));
        }
        const auto reference_digest = assets::Sha256Digest::from_hex(reference_digest_text.value());
        if (!reference_digest) return core::Result<void>::failure(reference_digest.error());
        if (reference_digest.value() != reference_genome.source_digest.value()) {
            return core::Result<void>::failure(validation(
                "GTF reference digest does not match the supplied reference genome"));
        }
        const auto assembly = decode_hex_text(assembly_hex.value(), "GTF assembly", 256U, true);
        const auto sample = decode_hex_text(
            sample_reference_hex.value(), "GTF sample reference", kMaxReferenceBytes, false);
        const auto source = decode_hex_text(
            provenance_source_hex.value(), "GTF provenance source", kMaxReferenceBytes, true);
        const auto release = decode_hex_text(
            provenance_release_hex.value(), "GTF provenance release", kMaxTextBytes, false);
        const auto license = decode_hex_text(
            provenance_license_hex.value(), "GTF provenance license", kMaxTextBytes, false);
        const auto provider = decode_hex_text(
            provenance_provider_hex.value(), "GTF provenance provider", kMaxTextBytes, false);
        const auto imported_at = decode_hex_text(
            provenance_imported_at_hex.value(), "GTF provenance imported_at", 128U, false);
        if (!assembly || !sample || !source || !release || !license || !provider || !imported_at) {
            return core::Result<void>::failure(parse_error(
                "GTF provenance or identity metadata is malformed"));
        }
        if (assembly.value() != reference_genome.assembly ||
            sample.value() != reference_genome.sample_reference) {
            return core::Result<void>::failure(validation(
                "GTF assembly or sample identity does not match the supplied reference genome"));
        }
        return core::Result<void>::success();
    };

    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.size() > 4U * 1024U * 1024U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GTF contains an empty or oversized line"));
        }
        if (line_number == 1U && line != "#!cartographer.gtf_version=2.2") {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GTF must begin with #!cartographer.gtf_version=2.2"));
        }
        if (line.starts_with("#")) {
            if (body_seen) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GTF metadata appears after a feature record"));
            }
            constexpr std::string_view kMetadataPrefix = "#!cartographer.";
            if (!line.starts_with(kMetadataPrefix)) {
                return core::Result<SequenceGenome>::failure(validation(
                    "GTF contains unsupported metadata that the bounded adapter would discard"));
            }
            const auto separator = line.find('=', kMetadataPrefix.size());
            if (separator == std::string::npos) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GTF Cartographer metadata is missing its value separator"));
            }
            const auto key = std::string_view(line).substr(
                kMetadataPrefix.size(), separator - kMetadataPrefix.size());
            const auto value = std::string_view(line).substr(separator + 1U);
            core::Result<void> result = core::Result<void>::failure(parse_error(
                "unknown Cartographer GTF metadata key: " + std::string(key)));
            if (key == "gtf_version") {
                if (version_seen) result = core::Result<void>::failure(parse_error("gtf_version is duplicated"));
                else if (value != "2.2") result = core::Result<void>::failure(validation("GTF version metadata must be 2.2"));
                else { version_seen = true; result = core::Result<void>::success(); }
            } else if (key == "reference_digest") result = set_metadata(reference_digest_text, value, key);
            else if (key == "assembly_hex") result = set_metadata(assembly_hex, value, key);
            else if (key == "sample_reference_hex") result = set_metadata(sample_reference_hex, value, key);
            else if (key == "coordinate_system") result = set_metadata(coordinate_system, value, key);
            else if (key == "feature_identity") result = set_metadata(feature_identity, value, key);
            else if (key == "provenance_source_hex") result = set_metadata(provenance_source_hex, value, key);
            else if (key == "provenance_release_hex") result = set_metadata(provenance_release_hex, value, key);
            else if (key == "provenance_license_hex") result = set_metadata(provenance_license_hex, value, key);
            else if (key == "provenance_provider_hex") result = set_metadata(provenance_provider_hex, value, key);
            else if (key == "provenance_imported_at_hex") result = set_metadata(provenance_imported_at_hex, value, key);
            if (!result) return core::Result<SequenceGenome>::failure(result.error());
            continue;
        }
        if (!body_seen) {
            if (auto result = metadata_ready(); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
        }
        body_seen = true;
        const auto fields = split_vcf_fields(line);
        if (fields.size() != 9U) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GTF records must contain exactly nine columns"));
        }
        const auto contig = contigs_by_name.find(std::string(fields[0]));
        if (contig == contigs_by_name.end()) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF record references a contig absent from the supplied reference genome"));
        }
        if (auto result = validate_gff_token(fields[1] == "." ? "" : fields[1],
                                              "GTF feature source", 256U, false); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        if (auto result = validate_gff_token(fields[2], "GTF feature type", 256U); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
        std::uint64_t start_one_based = 0U;
        std::uint64_t end_one_based = 0U;
        const auto parsed_start = std::from_chars(
            fields[3].data(), fields[3].data() + fields[3].size(), start_one_based);
        const auto parsed_end = std::from_chars(
            fields[4].data(), fields[4].data() + fields[4].size(), end_one_based);
        if (parsed_start.ec != std::errc{} || parsed_start.ptr != fields[3].data() + fields[3].size() ||
            parsed_end.ec != std::errc{} || parsed_end.ptr != fields[4].data() + fields[4].size() ||
            start_one_based == 0U || end_one_based < start_one_based) {
            return core::Result<SequenceGenome>::failure(parse_error(
                "GTF coordinates must be positive one-based inclusive integers"));
        }
        const auto& reference_contig = reference_genome.contigs_.at(contig->second);
        if (end_one_based > reference_contig.sequence.size()) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF feature interval lies outside its reference contig"));
        }
        std::optional<double> score;
        if (fields[5] != ".") {
            const auto parsed_score = parse_gff_double(fields[5], "GTF score");
            if (!parsed_score) return core::Result<SequenceGenome>::failure(parsed_score.error());
            score = parsed_score.value();
        }
        if (fields[6] != "." && fields[6] != "+" && fields[6] != "-" && fields[6] != "?") {
            return core::Result<SequenceGenome>::failure(validation("GTF strand is unsupported"));
        }
        std::optional<std::uint8_t> phase;
        if (fields[7] != ".") {
            if (fields[7].size() != 1U || fields[7][0] < '0' || fields[7][0] > '2') {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GTF frame must be '.', '0', '1', or '2'"));
            }
            phase = static_cast<std::uint8_t>(fields[7][0] - '0');
        }

        std::string source_identifier;
        std::optional<std::uint64_t> explicit_id;
        std::vector<SequenceFeatureAttribute> attributes;
        std::set<std::string> attribute_keys;
        std::size_t cursor = 0U;
        while (cursor < fields[8].size()) {
            while (cursor < fields[8].size() &&
                   (fields[8][cursor] == ' ' || fields[8][cursor] == '\t')) ++cursor;
            if (cursor == fields[8].size()) break;
            const auto key_start = cursor;
            while (cursor < fields[8].size() && fields[8][cursor] != ' ' &&
                   fields[8][cursor] != '\t') ++cursor;
            const auto key = fields[8].substr(key_start, cursor - key_start);
            if (auto result = validate_gff_token(key, "GTF attribute key", 128U); !result) {
                return core::Result<SequenceGenome>::failure(result.error());
            }
            while (cursor < fields[8].size() &&
                   (fields[8][cursor] == ' ' || fields[8][cursor] == '\t')) ++cursor;
            if (cursor >= fields[8].size() || fields[8][cursor] != '"') {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GTF attribute value must be quoted"));
            }
            ++cursor;
            const auto value_start = cursor;
            bool escaped = false;
            while (cursor < fields[8].size()) {
                if (!escaped && fields[8][cursor] == '"') break;
                if (!escaped && fields[8][cursor] == '\\') escaped = true;
                else escaped = false;
                ++cursor;
            }
            if (cursor >= fields[8].size()) {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GTF attribute value is missing its closing quote"));
            }
            const auto encoded_value = fields[8].substr(value_start, cursor - value_start);
            ++cursor;
            while (cursor < fields[8].size() &&
                   (fields[8][cursor] == ' ' || fields[8][cursor] == '\t')) ++cursor;
            if (cursor >= fields[8].size() || fields[8][cursor] != ';') {
                return core::Result<SequenceGenome>::failure(parse_error(
                    "GTF attribute must end with a semicolon"));
            }
            ++cursor;
            if (!attribute_keys.insert(std::string(key)).second) {
                return core::Result<SequenceGenome>::failure(validation(
                    "GTF attribute keys must be unique"));
            }
            const auto decoded = gtf_unescape(
                encoded_value, "GTF attribute value", 16U * 1024U, false);
            if (!decoded) return core::Result<SequenceGenome>::failure(decoded.error());
            if (key == "CARTOGRAPHER_ID") {
                std::uint64_t value = 0U;
                const auto parsed = std::from_chars(
                    decoded.value().data(), decoded.value().data() + decoded.value().size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != decoded.value().data() + decoded.value().size() || value == 0U) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "GTF Cartographer feature id is invalid"));
                }
                explicit_id = value;
            } else if (key == "cartographer_source_id") {
                if (decoded.value().empty()) {
                    return core::Result<SequenceGenome>::failure(parse_error(
                        "GTF Cartographer source identifier must be non-empty"));
                }
                source_identifier = decoded.value();
            } else {
                attributes.push_back({std::string(key), decoded.value()});
            }
        }
        if (!source_identifier.empty() && !source_identifiers.insert(source_identifier).second) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF source identifiers must be unique"));
        }
        const auto canonical_id = feature_identity_key(
            fields[0], start_one_based - 1U, end_one_based, fields[2], source_identifier, fields[8]);
        const SequenceFeatureId feature_id{
            explicit_id.value_or(stable_feature_id(canonical_id))};
        if (!imported_ids.insert(feature_id).second || reference_genome.features_.contains(feature_id)) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF feature identifiers collide after canonicalization"));
        }
        imported_features.push_back(SequenceFeature{
            feature_id, contig->second, start_one_based - 1U, end_one_based,
            std::string(fields[2]), fields[1] == "." ? std::string{} : std::string(fields[1]),
            score, std::string(fields[6]), phase, std::move(source_identifier),
            std::move(attributes), provenance});
        if (imported_features.size() > kMaxCollectionEntries) {
            return core::Result<SequenceGenome>::failure(validation(
                "GTF feature collection exceeds the safety limit"));
        }
    }
    if (input.bad() || !version_seen || !body_seen) {
        return core::Result<SequenceGenome>::failure(parse_error(
            "GTF is missing its required version or feature records"));
    }
    if (auto result = metadata_ready(); !result) {
        return core::Result<SequenceGenome>::failure(result.error());
    }
    for (auto& feature : imported_features) {
        if (auto result = reference_genome.insert_feature(std::move(feature)); !result) {
            return core::Result<SequenceGenome>::failure(result.error());
        }
    }
    if (auto result = reference_genome.validate(); !result) {
        return core::Result<SequenceGenome>::failure(
            result.error().with_context("GTF imported sequence genome"));
    }
    return core::Result<SequenceGenome>::success(std::move(reference_genome));
}

namespace {

std::string morphology_reference_canonical(const MorphologyModel& model) {
    std::ostringstream output;
    output << std::quoted(model.organism_identity) << ' '
           << std::quoted(model.body_plan) << ' ' << std::quoted(model.symmetry) << ' '
           << model.axes.size();
    for (const auto& axis : model.axes) output << ' ' << std::quoted(axis);
    output << ' ' << model.structures().size();
    for (const auto& [id, structure] : model.structures()) {
        output << ' ' << id.value << ' ' << optional_id(structure.parent_structure) << ' '
               << std::quoted(structure.semantic_class) << ' '
               << std::quoted(structure.laterality) << ' '
               << std::quoted(structure.developmental_origin) << ' '
               << std::quoted(structure.geometry.kind) << ' '
               << std::quoted(structure.geometry.reference) << ' '
               << std::quoted(structure.local_frame) << ' '
               << std::quoted(structure.confidence);
    }
    return output.str();
}

assets::Sha256Digest morphology_reference_digest(const MorphologyModel& model) {
    const auto canonical = morphology_reference_canonical(model);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(canonical.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, canonical.size()});
}

core::Result<double> parse_landmark_double(
    std::string_view token,
    std::string_view field) {
    std::istringstream input{std::string(token)};
    double value = 0.0;
    std::string trailing;
    if (!(input >> value) || !std::isfinite(value) || (input >> trailing)) {
        return core::Result<double>::failure(parse_error(
            std::string(field) + " is not a finite decimal value"));
    }
    return core::Result<double>::success(value);
}

} // namespace

core::Result<MorphologyLandmarkSetExport> MorphologyModel::export_landmark_set() const {
    if (auto result = validate(); !result) {
        return core::Result<MorphologyLandmarkSetExport>::failure(
            result.error().with_context("landmark-set export"));
    }
    if (landmarks_.empty()) {
        return core::Result<MorphologyLandmarkSetExport>::failure(validation(
            "landmark-set export requires at least one landmark"));
    }

    const MorphologyLandmark* provenance_landmark = nullptr;
    std::optional<std::string> coordinate_frame;
    bool mixed_coordinate_frames = false;
    for (const auto& [id, landmark] : landmarks_) {
        static_cast<void>(id);
        if (provenance_landmark == nullptr && !landmark.provenance.source_reference.empty()) {
            provenance_landmark = &landmark;
        }
        if (!coordinate_frame.has_value()) coordinate_frame = landmark.coordinate_frame;
        else if (coordinate_frame.value() != landmark.coordinate_frame) mixed_coordinate_frames = true;
    }
    if (provenance_landmark == nullptr) {
        return core::Result<MorphologyLandmarkSetExport>::failure(validation(
            "landmark-set export requires a landmark provenance source"));
    }

    constexpr std::size_t kMaxLandmarkSetBytes = 16U * 1024U * 1024U;
    std::ostringstream output;
    output << "##fileformat=cartographer.landmarks.v1\n"
           << "##cartographer.reference_digest=" << morphology_reference_digest(*this).hex() << '\n'
           << "##cartographer.organism_hex=" << encode_hex_text(organism_identity) << '\n'
           << "##cartographer.body_plan_hex=" << encode_hex_text(body_plan) << '\n'
           << "##cartographer.symmetry_hex=" << encode_hex_text(symmetry) << '\n'
           << "##cartographer.coordinate_frame_hex="
           << (mixed_coordinate_frames ? "mixed" : encode_hex_text(coordinate_frame.value())) << '\n'
           << "##cartographer.provenance_source_hex="
           << encode_hex_text(provenance_landmark->provenance.source_reference) << '\n'
           << "##cartographer.provenance_release_hex="
           << encode_hex_text(provenance_landmark->provenance.release) << '\n'
           << "##cartographer.provenance_license_hex="
           << encode_hex_text(provenance_landmark->provenance.license) << '\n'
           << "##cartographer.provenance_provider_hex="
           << encode_hex_text(provenance_landmark->provenance.provider) << '\n'
           << "##cartographer.provenance_imported_at_hex="
           << encode_hex_text(provenance_landmark->provenance.imported_at_utc) << '\n'
           << "#LANDMARK_ID\tSTRUCTURE_ID\tNAME_HEX\tX\tY\tZ\tFRAME_HEX\n";
    for (const auto& [id, landmark] : landmarks_) {
        output << id.value << '\t' << optional_id(landmark.structure) << '\t'
               << encode_hex_text(landmark.name) << '\t'
               << std::setprecision(17) << landmark.position.x << '\t'
               << std::setprecision(17) << landmark.position.y << '\t'
               << std::setprecision(17) << landmark.position.z << '\t'
               << encode_hex_text(landmark.coordinate_frame) << '\n';
        if (output.tellp() < 0 || static_cast<std::size_t>(output.tellp()) > kMaxLandmarkSetBytes) {
            return core::Result<MorphologyLandmarkSetExport>::failure(validation(
                "landmark-set export exceeds the safety limit"));
        }
    }

    MorphologyLandmarkSetExport result;
    result.text = output.str();
    result.feature_loss_notes.push_back(
        "landmark exchange omits per-landmark provenance and provider-specific geometry attachments");
    result.feature_loss_notes.push_back(
        "landmark exchange omits morphology attachments, growth domains, and solver/provider results");
    return core::Result<MorphologyLandmarkSetExport>::success(std::move(result));
}

core::Result<MorphologyModel> MorphologyModel::import_landmark_set(
    std::string_view text,
    MorphologyModel reference_model,
    Provenance provenance) {
    constexpr std::size_t kMaxLandmarkSetBytes = 16U * 1024U * 1024U;
    if (text.empty() || text.size() > kMaxLandmarkSetBytes) {
        return core::Result<MorphologyModel>::failure(parse_error(
            "landmark-set input is empty or exceeds the safety limit"));
    }
    if (auto result = provenance.validate(); !result) {
        return core::Result<MorphologyModel>::failure(
            result.error().with_context("landmark-set provenance"));
    }
    if (provenance.source_reference.empty()) {
        return core::Result<MorphologyModel>::failure(validation(
            "landmark-set import requires a provenance source reference"));
    }
    if (auto result = reference_model.validate(); !result) {
        return core::Result<MorphologyModel>::failure(
            result.error().with_context("landmark-set reference model"));
    }
    if (!reference_model.landmarks_.empty()) {
        return core::Result<MorphologyModel>::failure(validation(
            "landmark-set import requires a reference model without landmarks"));
    }
    for (const auto& [id, structure] : reference_model.structures_) {
        static_cast<void>(id);
        if (!structure.landmarks.empty()) {
            return core::Result<MorphologyModel>::failure(validation(
                "landmark-set import requires reference structures without landmark bindings"));
        }
    }

    bool fileformat_seen = false;
    bool column_header_seen = false;
    std::optional<std::string> reference_digest_text;
    std::optional<std::string> organism_hex;
    std::optional<std::string> body_plan_hex;
    std::optional<std::string> symmetry_hex;
    std::optional<std::string> coordinate_frame_hex;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    std::vector<MorphologyLandmark> imported_landmarks;
    std::set<LandmarkId> imported_ids;

    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };
    const auto metadata_ready = [&]() -> core::Result<void> {
        if (!fileformat_seen || !reference_digest_text.has_value() || !organism_hex.has_value() ||
            !body_plan_hex.has_value() || !symmetry_hex.has_value() ||
            !coordinate_frame_hex.has_value() || !provenance_source_hex.has_value() ||
            !provenance_release_hex.has_value() || !provenance_license_hex.has_value() ||
            !provenance_provider_hex.has_value() || !provenance_imported_at_hex.has_value()) {
            return core::Result<void>::failure(parse_error(
                "landmark-set is missing required identity or provenance metadata"));
        }
        const auto reference_digest = assets::Sha256Digest::from_hex(reference_digest_text.value());
        if (!reference_digest) return core::Result<void>::failure(reference_digest.error());
        if (reference_digest.value() != morphology_reference_digest(reference_model)) {
            return core::Result<void>::failure(validation(
                "landmark-set reference digest does not match the supplied morphology model"));
        }
        const auto organism = decode_hex_text(organism_hex.value(), "landmark organism", 256U, false);
        const auto body_plan = decode_hex_text(body_plan_hex.value(), "landmark body plan", 256U, false);
        const auto symmetry = decode_hex_text(symmetry_hex.value(), "landmark symmetry", 128U, false);
        const auto source = decode_hex_text(
            provenance_source_hex.value(), "landmark provenance source", kMaxReferenceBytes, true);
        const auto release = decode_hex_text(
            provenance_release_hex.value(), "landmark provenance release", kMaxTextBytes, false);
        const auto license = decode_hex_text(
            provenance_license_hex.value(), "landmark provenance license", kMaxTextBytes, false);
        const auto provider = decode_hex_text(
            provenance_provider_hex.value(), "landmark provenance provider", kMaxTextBytes, false);
        const auto imported_at = decode_hex_text(
            provenance_imported_at_hex.value(), "landmark provenance imported_at", 128U, false);
        if (!organism || !body_plan || !symmetry || !source || !release || !license || !provider ||
            !imported_at) {
            return core::Result<void>::failure(parse_error(
                "landmark-set identity or provenance metadata is malformed"));
        }
        if (organism.value() != reference_model.organism_identity ||
            body_plan.value() != reference_model.body_plan ||
            symmetry.value() != reference_model.symmetry) {
            return core::Result<void>::failure(validation(
                "landmark-set identity does not match the supplied morphology model"));
        }
        if (coordinate_frame_hex.value() != "mixed") {
            const auto frame = decode_hex_text(
                coordinate_frame_hex.value(), "landmark coordinate frame", 256U, true);
            if (!frame) return core::Result<void>::failure(frame.error());
        }
        return core::Result<void>::success();
    };

    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.size() > 4U * 1024U * 1024U) {
            return core::Result<MorphologyModel>::failure(parse_error(
                "landmark-set contains an empty or oversized line"));
        }
        if (line_number == 1U && line != "##fileformat=cartographer.landmarks.v1") {
            return core::Result<MorphologyModel>::failure(parse_error(
                "landmark-set must begin with cartographer.landmarks.v1"));
        }
        if (line.starts_with("##")) {
            if (column_header_seen) {
                return core::Result<MorphologyModel>::failure(parse_error(
                    "landmark-set metadata appears after the column header"));
            }
            if (line == "##fileformat=cartographer.landmarks.v1") {
                if (fileformat_seen) {
                    return core::Result<MorphologyModel>::failure(parse_error(
                        "landmark-set fileformat metadata is duplicated"));
                }
                fileformat_seen = true;
                continue;
            }
            constexpr std::string_view kMetadataPrefix = "##cartographer.";
            if (!line.starts_with(kMetadataPrefix)) {
                return core::Result<MorphologyModel>::failure(validation(
                    "landmark-set contains unsupported metadata that the bounded adapter would discard"));
            }
            const auto separator = line.find('=', kMetadataPrefix.size());
            if (separator == std::string::npos) {
                return core::Result<MorphologyModel>::failure(parse_error(
                    "landmark-set metadata is missing its value separator"));
            }
            const auto key = std::string_view(line).substr(
                kMetadataPrefix.size(), separator - kMetadataPrefix.size());
            const auto value = std::string_view(line).substr(separator + 1U);
            core::Result<void> result = core::Result<void>::failure(parse_error(
                "unknown landmark-set metadata key: " + std::string(key)));
            if (key == "reference_digest") result = set_metadata(reference_digest_text, value, key);
            else if (key == "organism_hex") result = set_metadata(organism_hex, value, key);
            else if (key == "body_plan_hex") result = set_metadata(body_plan_hex, value, key);
            else if (key == "symmetry_hex") result = set_metadata(symmetry_hex, value, key);
            else if (key == "coordinate_frame_hex") result = set_metadata(coordinate_frame_hex, value, key);
            else if (key == "provenance_source_hex") result = set_metadata(provenance_source_hex, value, key);
            else if (key == "provenance_release_hex") result = set_metadata(provenance_release_hex, value, key);
            else if (key == "provenance_license_hex") result = set_metadata(provenance_license_hex, value, key);
            else if (key == "provenance_provider_hex") result = set_metadata(provenance_provider_hex, value, key);
            else if (key == "provenance_imported_at_hex") result = set_metadata(provenance_imported_at_hex, value, key);
            if (!result) return core::Result<MorphologyModel>::failure(result.error());
            continue;
        }
        if (line == "#LANDMARK_ID\tSTRUCTURE_ID\tNAME_HEX\tX\tY\tZ\tFRAME_HEX") {
            if (!fileformat_seen || column_header_seen) {
                return core::Result<MorphologyModel>::failure(parse_error(
                    "landmark-set column header is duplicated or misplaced"));
            }
            if (auto result = metadata_ready(); !result) {
                return core::Result<MorphologyModel>::failure(result.error());
            }
            column_header_seen = true;
            continue;
        }
        if (!column_header_seen || line.front() == '#') {
            return core::Result<MorphologyModel>::failure(parse_error(
                "landmark-set contains an unexpected header or record line"));
        }
        const auto fields = split_vcf_fields(line);
        if (fields.size() != 7U) {
            return core::Result<MorphologyModel>::failure(parse_error(
                "landmark-set records must contain exactly seven columns"));
        }
        const auto parse_id = [](std::string_view token, std::string_view field)
            -> core::Result<std::uint64_t> {
            std::uint64_t value = 0U;
            const auto parsed = std::from_chars(
                token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
                value == 0U) {
                return core::Result<std::uint64_t>::failure(parse_error(
                    std::string(field) + " is not a non-zero unsigned integer"));
            }
            return core::Result<std::uint64_t>::success(value);
        };
        const auto landmark_id = parse_id(fields[0], "landmark id");
        if (!landmark_id) return core::Result<MorphologyModel>::failure(landmark_id.error());
        std::optional<StructureId> structure;
        if (fields[1] != "0") {
            const auto structure_id = parse_id(fields[1], "landmark structure id");
            if (!structure_id) return core::Result<MorphologyModel>::failure(structure_id.error());
            structure = StructureId{structure_id.value()};
            if (!reference_model.structures_.contains(structure.value())) {
                return core::Result<MorphologyModel>::failure(validation(
                    "landmark-set references a structure absent from the supplied model"));
            }
        }
        const auto name = decode_hex_text(fields[2], "landmark name", 256U, true);
        const auto x = parse_landmark_double(fields[3], "landmark x");
        const auto y = parse_landmark_double(fields[4], "landmark y");
        const auto z = parse_landmark_double(fields[5], "landmark z");
        const auto frame = decode_hex_text(fields[6], "landmark coordinate frame", 256U, true);
        if (!name || !x || !y || !z || !frame) {
            return core::Result<MorphologyModel>::failure(parse_error(
                "invalid landmark-set record"));
        }
        if (coordinate_frame_hex.value() != "mixed") {
            const auto expected_frame = decode_hex_text(
                coordinate_frame_hex.value(), "landmark coordinate frame", 256U, true);
            if (!expected_frame || expected_frame.value() != frame.value()) {
                return core::Result<MorphologyModel>::failure(validation(
                    "landmark-set record coordinate frame disagrees with its envelope"));
            }
        }
        const LandmarkId id{landmark_id.value()};
        if (!imported_ids.insert(id).second || reference_model.landmarks_.contains(id)) {
            return core::Result<MorphologyModel>::failure(validation(
                "landmark-set contains a duplicate or colliding landmark id"));
        }
        if (imported_landmarks.size() >= kMaxCollectionEntries) {
            return core::Result<MorphologyModel>::failure(validation(
                "landmark-set collection exceeds the safety limit"));
        }
        imported_landmarks.push_back(MorphologyLandmark{
            id, name.value(), structure, {x.value(), y.value(), z.value()}, frame.value(), provenance});
    }
    if (input.bad() || !column_header_seen) {
        return core::Result<MorphologyModel>::failure(parse_error(
            "landmark-set is missing its required column header"));
    }
    if (auto result = metadata_ready(); !result) {
        return core::Result<MorphologyModel>::failure(result.error());
    }
    if (imported_landmarks.empty()) {
        return core::Result<MorphologyModel>::failure(parse_error(
            "landmark-set contains no landmark records"));
    }
    for (const auto& landmark : imported_landmarks) {
        if (auto result = reference_model.insert_landmark(landmark); !result) {
            return core::Result<MorphologyModel>::failure(result.error());
        }
        if (landmark.structure.has_value()) {
            if (auto result = reference_model.attach_landmark_to_structure(
                    landmark.id, *landmark.structure); !result) {
                return core::Result<MorphologyModel>::failure(result.error());
            }
        }
    }
    if (auto result = reference_model.validate(); !result) {
        return core::Result<MorphologyModel>::failure(
            result.error().with_context("imported landmark-set model"));
    }
    return core::Result<MorphologyModel>::success(std::move(reference_model));
}

core::Result<void> AnatomyTerm::validate() const {
    if (auto result = validate_id(id.value, "anatomy term id"); !result) return result;
    if (auto result = validate_text(namespace_name, "anatomy term namespace", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_text(accession, "anatomy term accession", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_text(label, "anatomy term label", 256U, true); !result) return result;
    if (auto result = validate_text(category, "anatomy term category", 128U, true); !result) return result;
    if (parent_term.has_value() && *parent_term == id) {
        return core::Result<void>::failure(validation("anatomy term cannot parent itself"));
    }
    if (auto result = validate_text(definition, "anatomy term definition", kMaxTextBytes); !result) {
        return result;
    }
    if (auto result = validate_text(ontology_version, "anatomy term ontology version", 256U);
        !result) {
        return result;
    }
    if (auto result = validate_text(source_release, "anatomy term source release", 256U);
        !result) {
        return result;
    }
    return provenance.validate();
}

core::Result<void> AnatomyPart::validate() const {
    if (auto result = validate_id(id.value, "anatomy part id"); !result) return result;
    if (auto result = validate_id(structure.value, "anatomy part structure"); !result) return result;
    if (auto result = validate_id(term.value, "anatomy part term"); !result) return result;
    if (parent_part.has_value() && *parent_part == id) {
        return core::Result<void>::failure(validation("anatomy part cannot parent itself"));
    }
    if (auto result = validate_text(role, "anatomy part role", 256U, true); !result) return result;
    return provenance.validate();
}

core::Result<void> TissueRegion::validate() const {
    if (auto result = validate_id(id.value, "tissue region id"); !result) return result;
    if (auto result = validate_text(tissue_type, "tissue region type", 256U, true); !result) {
        return result;
    }
    if (structure.has_value() && !structure->value) {
        return core::Result<void>::failure(validation(
            "tissue region structure must be non-zero"));
    }
    if (region.has_value() && !region->value) {
        return core::Result<void>::failure(validation(
            "tissue region region must be non-zero"));
    }
    if (auto result = validate_text(physical_properties, "tissue region physical properties",
                                    kMaxTextBytes); !result) {
        return result;
    }
    if (auto result = validate_text(developmental_origin, "tissue region developmental origin",
                                    kMaxReferenceBytes); !result) {
        return result;
    }
    if (auto result = validate_text(stage, "tissue region stage", 256U, true); !result) {
        return result;
    }
    if (auto result = geometry.validate(); !result) return result;
    return provenance.validate();
}

core::Result<void> AnatomyRelation::validate() const {
    if (auto result = validate_id(id.value, "anatomy relation id"); !result) return result;
    if (auto result = validate_id(first_part.value, "anatomy relation first part"); !result) return result;
    if (auto result = validate_id(second_part.value, "anatomy relation second part"); !result) return result;
    if (first_part == second_part) {
        return core::Result<void>::failure(validation("anatomy relation parts must be distinct"));
    }
    if (auto result = validate_text(relation, "anatomy relation kind", 128U, true); !result) return result;
    return provenance.validate();
}

core::Result<void> AnatomyModel::insert_term(AnatomyTerm value) {
    return insert_unique(terms_, std::move(value), "anatomy term");
}

core::Result<void> AnatomyModel::insert_part(AnatomyPart value) {
    return insert_unique(parts_, std::move(value), "anatomy part");
}

core::Result<void> AnatomyModel::insert_tissue_region(TissueRegion value) {
    return insert_unique(tissue_regions_, std::move(value), "tissue region");
}

core::Result<void> AnatomyModel::insert_relation(AnatomyRelation value) {
    return insert_unique(relations_, std::move(value), "anatomy relation");
}

core::Result<void> AnatomyModel::validate() const {
    if (auto result = validate_text(schema, "anatomy schema", 128U, true); !result) return result;
    if (terms_.size() > kMaxCollectionEntries || parts_.size() > kMaxCollectionEntries ||
        tissue_regions_.size() > kMaxCollectionEntries ||
        relations_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "anatomy collection exceeds the safety limit"));
    }
    for (const auto& [id, term] : terms_) {
        static_cast<void>(id);
        if (auto result = term.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("anatomy term"));
        }
        if (auto result = validate_optional_reference(term.parent_term, "anatomy term parent", terms_);
            !result) return result;
    }
    std::map<AnatomyTermId, std::uint8_t> term_visit;
    const auto visit_term = [&](const auto& self, AnatomyTermId id) -> core::Result<void> {
        const auto state = term_visit[id];
        if (state == 1U) return core::Result<void>::failure(validation("anatomy term hierarchy contains a cycle"));
        if (state == 2U) return core::Result<void>::success();
        term_visit[id] = 1U;
        const auto& term = terms_.at(id);
        if (term.parent_term.has_value()) {
            if (auto result = self(self, *term.parent_term); !result) return result;
        }
        term_visit[id] = 2U;
        return core::Result<void>::success();
    };
    for (const auto& [id, term] : terms_) {
        static_cast<void>(term);
        if (auto result = visit_term(visit_term, id); !result) return result;
    }
    for (const auto& [id, part] : parts_) {
        static_cast<void>(id);
        if (auto result = part.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("anatomy part"));
        }
        if (auto result = validate_reference(terms_, part.term, "anatomy part term"); !result) return result;
        if (auto result = validate_optional_reference(part.parent_part, "anatomy part parent", parts_);
            !result) return result;
    }
    for (const auto& [id, tissue_region] : tissue_regions_) {
        static_cast<void>(id);
        if (auto result = tissue_region.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("tissue region"));
        }
    }
    std::map<AnatomyPartId, std::uint8_t> part_visit;
    const auto visit_part = [&](const auto& self, AnatomyPartId id) -> core::Result<void> {
        const auto state = part_visit[id];
        if (state == 1U) return core::Result<void>::failure(validation("anatomy part hierarchy contains a cycle"));
        if (state == 2U) return core::Result<void>::success();
        part_visit[id] = 1U;
        const auto& part = parts_.at(id);
        if (part.parent_part.has_value()) {
            if (auto result = self(self, *part.parent_part); !result) return result;
        }
        part_visit[id] = 2U;
        return core::Result<void>::success();
    };
    for (const auto& [id, part] : parts_) {
        static_cast<void>(part);
        if (auto result = visit_part(visit_part, id); !result) return result;
    }
    for (const auto& [id, relation] : relations_) {
        static_cast<void>(id);
        if (auto result = relation.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("anatomy relation"));
        }
        if (auto result = validate_reference(parts_, relation.first_part, "anatomy relation first part");
            !result) return result;
        if (auto result = validate_reference(parts_, relation.second_part, "anatomy relation second part");
            !result) return result;
    }
    return core::Result<void>::success();
}

std::string AnatomyModel::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_ANATOMY " << kSchemaVersion << '\n';
    output << std::quoted(schema) << '\n';
    output << "TERMS " << terms_.size() << '\n';
    for (const auto& [id, term] : terms_) {
        output << "TERM " << id.value << ' ' << optional_id(term.parent_term) << ' '
               << std::quoted(term.namespace_name) << ' ' << std::quoted(term.accession) << ' '
               << std::quoted(term.label) << ' ' << std::quoted(term.category) << ' '
               << std::quoted(term.definition) << ' ';
        write_provenance(output, term.provenance);
        output << ' ' << std::quoted(term.ontology_version) << ' '
               << std::quoted(term.source_release);
        output << '\n';
    }
    output << "PARTS " << parts_.size() << '\n';
    for (const auto& [id, part] : parts_) {
        output << "PART " << id.value << ' ' << part.structure.value << ' ' << part.term.value << ' '
               << optional_id(part.parent_part) << ' ' << std::quoted(part.role) << ' ';
        write_provenance(output, part.provenance);
        output << '\n';
    }
    output << "TISSUE_REGIONS " << tissue_regions_.size() << '\n';
    for (const auto& [id, tissue_region] : tissue_regions_) {
        output << "TISSUE_REGION " << id.value << ' ' << optional_id(tissue_region.structure)
               << ' ' << optional_id(tissue_region.region) << ' '
               << std::quoted(tissue_region.tissue_type) << ' '
               << std::quoted(tissue_region.physical_properties) << ' '
               << std::quoted(tissue_region.developmental_origin) << ' '
               << std::quoted(tissue_region.stage) << ' '
               << std::quoted(tissue_region.geometry.kind) << ' '
               << std::quoted(tissue_region.geometry.reference) << ' ';
        write_provenance(output, tissue_region.provenance);
        output << '\n';
    }
    output << "RELATIONS " << relations_.size() << '\n';
    for (const auto& [id, relation] : relations_) {
        output << "RELATION " << id.value << ' ' << relation.first_part.value << ' '
               << relation.second_part.value << ' ' << std::quoted(relation.relation) << ' ';
        write_provenance(output, relation.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<AnatomyModel> AnatomyModel::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<AnatomyModel>::failure(parse_error("anatomy record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_ANATOMY"); !result) {
        return core::Result<AnatomyModel>::failure(result.error());
    }
    const auto version = read_uint(input, "anatomy schema version");
    if (!version || (version.value() != 1U && version.value() != 2U &&
                     version.value() != kSchemaVersion)) {
        return core::Result<AnatomyModel>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported anatomy schema version"));
    }
    const auto schema = read_string(input, "anatomy schema", 128U, true);
    if (!schema) return core::Result<AnatomyModel>::failure(schema.error());
    AnatomyModel model;
    model.schema = schema.value();
    if (auto result = require_record(input, "TERMS"); !result) {
        return core::Result<AnatomyModel>::failure(result.error());
    }
    const auto term_count = read_uint(input, "anatomy term count");
    if (!term_count || term_count.value() > kMaxCollectionEntries) {
        return core::Result<AnatomyModel>::failure(parse_error("invalid anatomy term count"));
    }
    for (std::uint64_t index = 0U; index < term_count.value(); ++index) {
        if (auto result = require_record(input, "TERM"); !result) {
            return core::Result<AnatomyModel>::failure(result.error());
        }
        const auto id = read_uint(input, "anatomy term id");
        const auto parent = read_optional_id<AnatomyTermId>(input, "anatomy term parent");
        const auto namespace_name = read_string(input, "anatomy term namespace", 256U, true);
        const auto accession = read_string(input, "anatomy term accession", 256U, true);
        const auto label = read_string(input, "anatomy term label", 256U, true);
        const auto category = read_string(input, "anatomy term category", 128U, true);
        const auto definition = read_string(input, "anatomy term definition", kMaxTextBytes);
        const auto provenance = read_provenance(input);
        std::string ontology_version;
        std::string source_release;
        if (version.value() >= 3U) {
            const auto parsed_ontology_version = read_string(
                input, "anatomy term ontology version", 256U);
            const auto parsed_source_release = read_string(
                input, "anatomy term source release", 256U);
            if (!parsed_ontology_version || !parsed_source_release) {
                return core::Result<AnatomyModel>::failure(parse_error(
                    "invalid anatomy term ontology metadata"));
            }
            ontology_version = parsed_ontology_version.value();
            source_release = parsed_source_release.value();
        }
        if (!id || !parent || !namespace_name || !accession || !label || !category || !definition ||
            !provenance) {
            return core::Result<AnatomyModel>::failure(parse_error("invalid anatomy term record"));
        }
        const auto inserted = model.insert_term(AnatomyTerm{
            AnatomyTermId{id.value()}, namespace_name.value(), accession.value(), label.value(),
            category.value(), parent.value(), definition.value(), provenance.value(),
            std::move(ontology_version), std::move(source_release)});
        if (!inserted) return core::Result<AnatomyModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "PARTS"); !result) {
        return core::Result<AnatomyModel>::failure(result.error());
    }
    const auto part_count = read_uint(input, "anatomy part count");
    if (!part_count || part_count.value() > kMaxCollectionEntries) {
        return core::Result<AnatomyModel>::failure(parse_error("invalid anatomy part count"));
    }
    for (std::uint64_t index = 0U; index < part_count.value(); ++index) {
        if (auto result = require_record(input, "PART"); !result) {
            return core::Result<AnatomyModel>::failure(result.error());
        }
        const auto id = read_uint(input, "anatomy part id");
        const auto structure = read_uint(input, "anatomy part structure");
        const auto term = read_uint(input, "anatomy part term");
        const auto parent = read_optional_id<AnatomyPartId>(input, "anatomy part parent");
        const auto role = read_string(input, "anatomy part role", 256U, true);
        const auto provenance = read_provenance(input);
        if (!id || !structure || !term || !parent || !role || !provenance) {
            return core::Result<AnatomyModel>::failure(parse_error("invalid anatomy part record"));
        }
        const auto inserted = model.insert_part(AnatomyPart{
            AnatomyPartId{id.value()}, StructureId{structure.value()}, AnatomyTermId{term.value()},
            parent.value(), role.value(), provenance.value()});
        if (!inserted) return core::Result<AnatomyModel>::failure(inserted.error());
    }
    if (version.value() >= 2U) {
        if (auto result = require_record(input, "TISSUE_REGIONS"); !result) {
            return core::Result<AnatomyModel>::failure(result.error());
        }
        const auto tissue_region_count = read_uint(input, "tissue region count");
        if (!tissue_region_count || tissue_region_count.value() > kMaxCollectionEntries) {
            return core::Result<AnatomyModel>::failure(parse_error(
                "invalid tissue region count"));
        }
        for (std::uint64_t index = 0U; index < tissue_region_count.value(); ++index) {
            if (auto result = require_record(input, "TISSUE_REGION"); !result) {
                return core::Result<AnatomyModel>::failure(result.error());
            }
            const auto id = read_uint(input, "tissue region id");
            const auto structure = read_optional_id<StructureId>(input, "tissue region structure");
            const auto region = read_optional_id<RegionId>(input, "tissue region region");
            const auto tissue_type = read_string(input, "tissue region type", 256U, true);
            const auto physical_properties = read_string(
                input, "tissue region physical properties", kMaxTextBytes);
            const auto developmental_origin = read_string(
                input, "tissue region developmental origin", kMaxReferenceBytes);
            const auto stage = read_string(input, "tissue region stage", 256U, true);
            const auto geometry_kind = read_string(input, "tissue region geometry kind", 128U);
            const auto geometry_reference = read_string(
                input, "tissue region geometry reference", kMaxReferenceBytes);
            const auto provenance = read_provenance(input);
            if (!id || !structure || !region || !tissue_type || !physical_properties ||
                !developmental_origin || !stage || !geometry_kind || !geometry_reference ||
                !provenance) {
                return core::Result<AnatomyModel>::failure(parse_error(
                    "invalid tissue region record"));
            }
            const auto inserted = model.insert_tissue_region(TissueRegion{
                TissueRegionId{id.value()}, tissue_type.value(), structure.value(), region.value(),
                physical_properties.value(), developmental_origin.value(),
                stage.value(), GeometryBinding{geometry_kind.value(), geometry_reference.value()},
                provenance.value()});
            if (!inserted) return core::Result<AnatomyModel>::failure(inserted.error());
        }
    }
    if (auto result = require_record(input, "RELATIONS"); !result) {
        return core::Result<AnatomyModel>::failure(result.error());
    }
    const auto relation_count = read_uint(input, "anatomy relation count");
    if (!relation_count || relation_count.value() > kMaxCollectionEntries) {
        return core::Result<AnatomyModel>::failure(parse_error("invalid anatomy relation count"));
    }
    for (std::uint64_t index = 0U; index < relation_count.value(); ++index) {
        if (auto result = require_record(input, "RELATION"); !result) {
            return core::Result<AnatomyModel>::failure(result.error());
        }
        const auto id = read_uint(input, "anatomy relation id");
        const auto first = read_uint(input, "anatomy relation first part");
        const auto second = read_uint(input, "anatomy relation second part");
        const auto relation = read_string(input, "anatomy relation kind", 128U, true);
        const auto provenance = read_provenance(input);
        if (!id || !first || !second || !relation || !provenance) {
            return core::Result<AnatomyModel>::failure(parse_error("invalid anatomy relation record"));
        }
        const auto inserted = model.insert_relation(AnatomyRelation{
            AnatomyRelationId{id.value()}, AnatomyPartId{first.value()}, AnatomyPartId{second.value()},
            relation.value(), provenance.value()});
        if (!inserted) return core::Result<AnatomyModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<AnatomyModel>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<AnatomyModel>::failure(parse_error("anatomy record contains trailing data"));
    }
    if (auto result = model.validate(); !result) {
        return core::Result<AnatomyModel>::failure(result.error());
    }
    return core::Result<AnatomyModel>::success(std::move(model));
}

core::Result<AnatomyOntologyTableExport> AnatomyModel::export_ontology_table() const {
    if (auto result = validate(); !result) {
        return core::Result<AnatomyOntologyTableExport>::failure(
            result.error().with_context("ontology-table export"));
    }
    if (terms_.empty()) {
        return core::Result<AnatomyOntologyTableExport>::failure(validation(
            "ontology-table export requires at least one anatomy term"));
    }
    const Provenance* envelope_provenance = nullptr;
    for (const auto& [id, term] : terms_) {
        static_cast<void>(id);
        if (envelope_provenance == nullptr && !term.provenance.source_reference.empty()) {
            envelope_provenance = &term.provenance;
        }
    }
    if (envelope_provenance == nullptr) {
        return core::Result<AnatomyOntologyTableExport>::failure(validation(
            "ontology-table export requires a term provenance source for its envelope"));
    }

    constexpr std::size_t kMaxOntologyTableBytes = 4U * 1024U * 1024U;
    std::ostringstream output;
    output << "#cartographer.ontology_table_version=1\n"
           << "#cartographer.schema_hex=" << encode_hex_text(schema) << '\n'
           << "#cartographer.term_identity=cartographer-anatomy-term-v1\n"
           << "#cartographer.columns=id\\tparent_id\\tnamespace_hex\\taccession_hex\\tlabel_hex\\tcategory_hex\\tdefinition_hex\\tontology_version_hex\\tsource_release_hex\n"
           << "#cartographer.provenance_source_hex="
           << encode_hex_text(envelope_provenance->source_reference) << '\n'
           << "#cartographer.provenance_release_hex="
           << encode_hex_text(envelope_provenance->release) << '\n'
           << "#cartographer.provenance_license_hex="
           << encode_hex_text(envelope_provenance->license) << '\n'
           << "#cartographer.provenance_provider_hex="
           << encode_hex_text(envelope_provenance->provider) << '\n'
           << "#cartographer.provenance_imported_at_hex="
           << encode_hex_text(envelope_provenance->imported_at_utc) << '\n';
    for (const auto& [id, term] : terms_) {
        output << id.value << '\t' << optional_id(term.parent_term) << '\t'
               << encode_hex_text(term.namespace_name) << '\t'
               << encode_hex_text(term.accession) << '\t'
               << encode_hex_text(term.label) << '\t'
               << encode_hex_text(term.category) << '\t'
               << encode_hex_text(term.definition) << '\t'
               << encode_hex_text(term.ontology_version) << '\t'
               << encode_hex_text(term.source_release) << '\n';
        if (output.tellp() < 0 ||
            static_cast<std::size_t>(output.tellp()) > kMaxOntologyTableBytes) {
            return core::Result<AnatomyOntologyTableExport>::failure(validation(
                "ontology-table export exceeds the safety limit"));
        }
    }
    AnatomyOntologyTableExport result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxOntologyTableBytes) {
        return core::Result<AnatomyOntologyTableExport>::failure(validation(
            "ontology-table export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "ontology tables preserve term identity, hierarchy, labels, definitions, categories, "
        "and ontology release metadata");
    result.feature_loss_notes.push_back(
        "ontology tables omit parts, tissue regions, relations, geometry bindings, and per-term provenance; "
        "import uses caller provenance");
    result.feature_loss_notes.push_back(
        "ontology labels remain semantic authoring data and do not become tissue behavior, geometry, "
        "phenotype, or VANTA runtime authority");
    return core::Result<AnatomyOntologyTableExport>::success(std::move(result));
}

core::Result<AnatomyModel> AnatomyModel::import_ontology_table(
    std::string_view text,
    AnatomyModel reference_model,
    Provenance provenance) {
    constexpr std::size_t kMaxOntologyTableBytes = 4U * 1024U * 1024U;
    if (text.size() > kMaxOntologyTableBytes) {
        return core::Result<AnatomyModel>::failure(parse_error(
            "ontology-table record is too large"));
    }
    if (auto result = provenance.validate(); !result || provenance.source_reference.empty()) {
        return core::Result<AnatomyModel>::failure(validation(
            "ontology-table import requires non-empty caller provenance"));
    }
    if (reference_model.schema.empty()) {
        if (!reference_model.terms_.empty() || !reference_model.parts_.empty() ||
            !reference_model.tissue_regions_.empty() || !reference_model.relations_.empty()) {
            return core::Result<AnatomyModel>::failure(validation(
                "ontology-table empty anatomy shell must not contain entities"));
        }
    } else if (auto result = reference_model.validate(); !result) {
        return core::Result<AnatomyModel>::failure(
            result.error().with_context("ontology-table reference anatomy"));
    }
    if (!reference_model.terms_.empty()) {
        return core::Result<AnatomyModel>::failure(validation(
            "ontology-table import requires a reference anatomy model without pre-existing terms"));
    }

    std::optional<std::string> version;
    std::optional<std::string> schema_hex;
    std::optional<std::string> term_identity;
    std::optional<std::string> columns;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    bool body_seen = false;
    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };
    const auto parse_uint = [](std::string_view token,
                               std::string_view field) -> core::Result<std::uint64_t> {
        if (token.empty()) {
            return core::Result<std::uint64_t>::failure(parse_error(
                std::string(field) + " is empty"));
        }
        std::uint64_t value = 0U;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            return core::Result<std::uint64_t>::failure(parse_error(
                std::string(field) + " is not an unsigned integer"));
        }
        return core::Result<std::uint64_t>::success(value);
    };
    const std::string expected_columns =
        "id\\tparent_id\\tnamespace_hex\\taccession_hex\\tlabel_hex\\tcategory_hex\\tdefinition_hex\\tontology_version_hex\\tsource_release_hex";
    std::vector<AnatomyTerm> imported_terms;
    std::set<std::string> ontology_keys;
    std::set<AnatomyTermId> imported_ids;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!body_seen && line.starts_with("#")) {
            core::Result<void> parsed = core::Result<void>::failure(parse_error(
                "unsupported ontology-table metadata header"));
            if (line.starts_with("#cartographer.ontology_table_version=")) {
                parsed = set_metadata(version, line.substr(
                    std::string_view("#cartographer.ontology_table_version=").size()),
                    "ontology-table version");
            } else if (line.starts_with("#cartographer.schema_hex=")) {
                parsed = set_metadata(schema_hex, line.substr(
                    std::string_view("#cartographer.schema_hex=").size()),
                    "ontology-table schema");
            } else if (line.starts_with("#cartographer.term_identity=")) {
                parsed = set_metadata(term_identity, line.substr(
                    std::string_view("#cartographer.term_identity=").size()),
                    "ontology-table term identity");
            } else if (line.starts_with("#cartographer.columns=")) {
                parsed = set_metadata(columns, line.substr(
                    std::string_view("#cartographer.columns=").size()),
                    "ontology-table columns");
            } else if (line.starts_with("#cartographer.provenance_source_hex=")) {
                parsed = set_metadata(provenance_source_hex, line.substr(
                    std::string_view("#cartographer.provenance_source_hex=").size()),
                    "ontology-table provenance source");
            } else if (line.starts_with("#cartographer.provenance_release_hex=")) {
                parsed = set_metadata(provenance_release_hex, line.substr(
                    std::string_view("#cartographer.provenance_release_hex=").size()),
                    "ontology-table provenance release");
            } else if (line.starts_with("#cartographer.provenance_license_hex=")) {
                parsed = set_metadata(provenance_license_hex, line.substr(
                    std::string_view("#cartographer.provenance_license_hex=").size()),
                    "ontology-table provenance license");
            } else if (line.starts_with("#cartographer.provenance_provider_hex=")) {
                parsed = set_metadata(provenance_provider_hex, line.substr(
                    std::string_view("#cartographer.provenance_provider_hex=").size()),
                    "ontology-table provenance provider");
            } else if (line.starts_with("#cartographer.provenance_imported_at_hex=")) {
                parsed = set_metadata(provenance_imported_at_hex, line.substr(
                    std::string_view("#cartographer.provenance_imported_at_hex=").size()),
                    "ontology-table provenance imported-at");
            }
            if (!parsed) return core::Result<AnatomyModel>::failure(parsed.error());
            continue;
        }
        if (line.starts_with("#")) {
            return core::Result<AnatomyModel>::failure(parse_error(
                "ontology-table comments or metadata after the term rows are unsupported"));
        }
        body_seen = true;
        const auto fields = split_vcf_fields(line);
        if (fields.size() != 9U) {
            return core::Result<AnatomyModel>::failure(parse_error(
                "ontology-table term row must contain exactly nine tab-separated fields"));
        }
        const auto id = parse_uint(fields[0], "ontology term id");
        const auto parent = parse_uint(fields[1], "ontology term parent id");
        const auto namespace_name = decode_hex_text(fields[2], "ontology term namespace", 256U, true);
        const auto accession = decode_hex_text(fields[3], "ontology term accession", 256U, true);
        const auto label = decode_hex_text(fields[4], "ontology term label", 256U, true);
        const auto category = decode_hex_text(fields[5], "ontology term category", 128U, true);
        const auto definition = decode_hex_text(fields[6], "ontology term definition", kMaxTextBytes, false);
        const auto ontology_version = decode_hex_text(
            fields[7], "ontology term ontology version", 256U, false);
        const auto source_release = decode_hex_text(
            fields[8], "ontology term source release", 256U, false);
        if (!id || !parent || !namespace_name || !accession || !label || !category ||
            !definition || !ontology_version || !source_release || id.value() == 0U) {
            return core::Result<AnatomyModel>::failure(parse_error(
                "ontology-table term row contains invalid fields"));
        }
        const AnatomyTermId term_id{id.value()};
        if (!imported_ids.insert(term_id).second) {
            return core::Result<AnatomyModel>::failure(validation(
                "ontology-table term IDs contain a duplicate"));
        }
        std::string ontology_key = namespace_name.value();
        ontology_key.push_back('\0');
        ontology_key.append(accession.value());
        if (!ontology_keys.insert(std::move(ontology_key)).second) {
            return core::Result<AnatomyModel>::failure(validation(
                "ontology-table namespace/accession pairs contain a duplicate"));
        }
        imported_terms.push_back(AnatomyTerm{
            term_id,
            namespace_name.value(),
            accession.value(),
            label.value(),
            category.value(),
            parent.value() == 0U ? std::nullopt : std::optional<AnatomyTermId>{AnatomyTermId{parent.value()}},
            definition.value(),
            provenance,
            ontology_version.value(),
            source_release.value()});
        if (imported_terms.size() > kMaxCollectionEntries) {
            return core::Result<AnatomyModel>::failure(parse_error(
                "ontology-table terms exceed the safety limit"));
        }
    }
    if (!body_seen || !version.has_value() || !schema_hex.has_value() ||
        !term_identity.has_value() || !columns.has_value() ||
        !provenance_source_hex.has_value() || !provenance_release_hex.has_value() ||
        !provenance_license_hex.has_value() || !provenance_provider_hex.has_value() ||
        !provenance_imported_at_hex.has_value()) {
        return core::Result<AnatomyModel>::failure(parse_error(
            "ontology-table is missing required identity or provenance metadata"));
    }
    if (version.value() != "1" || term_identity.value() != "cartographer-anatomy-term-v1" ||
        columns.value() != expected_columns) {
        return core::Result<AnatomyModel>::failure(validation(
            "ontology-table identity or columns metadata is unsupported"));
    }
    const auto table_schema = decode_hex_text(
        schema_hex.value(), "ontology-table schema", 128U, true);
    const auto envelope_source = decode_hex_text(
        provenance_source_hex.value(), "ontology-table provenance source", kMaxReferenceBytes, true);
    const auto envelope_release = decode_hex_text(
        provenance_release_hex.value(), "ontology-table provenance release", kMaxTextBytes, false);
    const auto envelope_license = decode_hex_text(
        provenance_license_hex.value(), "ontology-table provenance license", kMaxTextBytes, false);
    const auto envelope_provider = decode_hex_text(
        provenance_provider_hex.value(), "ontology-table provenance provider", kMaxTextBytes, false);
    const auto envelope_imported_at = decode_hex_text(
        provenance_imported_at_hex.value(), "ontology-table provenance imported-at", 128U, false);
    if (!table_schema || !envelope_source || !envelope_release || !envelope_license ||
        !envelope_provider || !envelope_imported_at) {
        return core::Result<AnatomyModel>::failure(parse_error(
            "ontology-table schema or provenance envelope is malformed"));
    }
    if ((!reference_model.schema.empty() && table_schema.value() != reference_model.schema) ||
        imported_terms.empty()) {
        return core::Result<AnatomyModel>::failure(validation(
            "ontology-table schema does not match the reference anatomy or has no terms"));
    }
    if (reference_model.schema.empty()) reference_model.schema = table_schema.value();
    for (auto& term : imported_terms) {
        if (auto result = reference_model.insert_term(std::move(term)); !result) {
            return core::Result<AnatomyModel>::failure(result.error());
        }
    }
    if (auto result = reference_model.validate(); !result) {
        return core::Result<AnatomyModel>::failure(
            result.error().with_context("imported ontology-table model"));
    }
    return core::Result<AnatomyModel>::success(std::move(reference_model));
}

core::Result<void> AnatomyProviderBinding::validate() const {
    if (auto result = validate_id(id.value, "anatomy provider binding id"); !result) return result;
    if (auto result = validate_id(part.value, "anatomy provider binding part"); !result) return result;
    if (auto result = validate_text(semantic_id, "anatomy provider semantic id", 256U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(laterality, "anatomy provider laterality", 128U, true);
        !result) {
        return result;
    }
    if (auto result = source_geometry.validate(); !result) return result;
    if (source_geometry.kind.empty() || source_geometry.reference.empty()) {
        return core::Result<void>::failure(validation(
            "anatomy provider binding requires source geometry lineage"));
    }
    if (auto result = validate_text(source_release, "anatomy provider source release", 256U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(license, "anatomy provider license", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_digest(geometry_digest, "anatomy provider geometry digest"); !result) {
        return result;
    }
    if (auto result = validate_text(tissue_material, "anatomy provider tissue material", 256U);
        !result) {
        return result;
    }
    if (auto result = validate_text(local_edit_reference,
                                    "anatomy provider local edit reference",
                                    kMaxReferenceBytes); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "anatomy provider binding provenance must be explicit"));
    }
    return provenance.validate();
}

core::Result<void> AnatomyProviderManifest::insert_binding(AnatomyProviderBinding value) {
    return insert_unique(bindings_, std::move(value), "anatomy provider binding");
}

core::Result<void> AnatomyProviderManifest::validate() const {
    if (auto result = validate_text(schema, "anatomy provider manifest schema", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(provider, "anatomy provider manifest provider", 256U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(publication_reference,
                                    "anatomy provider publication reference",
                                    kMaxReferenceBytes, true); !result) {
        return result;
    }
    if (auto result = validate_text(status, "anatomy provider publication status", 128U, true);
        !result) {
        return result;
    }
    if (source_model_revision.has_value() && source_model_revision->value() == 0U) {
        return core::Result<void>::failure(validation(
            "anatomy provider source model revision must be non-zero when present"));
    }
    if (auto result = validate_digest(source_manifest_digest,
                                      "anatomy provider source manifest digest"); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "anatomy provider manifest provenance must be explicit"));
    }
    if (auto result = provenance.validate(); !result) return result;
    if (bindings_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "anatomy provider binding collection exceeds the safety limit"));
    }
    for (const auto& [id, binding] : bindings_) {
        static_cast<void>(id);
        if (auto result = binding.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("anatomy provider binding"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> AnatomyProviderManifest::validate_handoff_candidate(
    core::Revision expected_source_revision) const {
    if (auto result = validate(); !result) return result;
    if (expected_source_revision.value() == 0U) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate expected source revision must be non-zero"));
    }
    if (!source_model_revision.has_value()) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate is missing its source model revision"));
    }
    if (*source_model_revision != expected_source_revision) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate source model revision is stale"));
    }
    if (!source_manifest_digest.has_value()) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate is missing its source manifest digest"));
    }
    if (bindings_.empty()) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate must contain at least one binding"));
    }
    for (const auto& [id, binding] : bindings_) {
        static_cast<void>(id);
        if (!binding.geometry_digest.has_value()) {
            return core::Result<void>::failure(validation(
                "anatomy provider candidate binding is missing its geometry digest"));
        }
    }
    return core::Result<void>::success();
}

std::string AnatomyProviderManifest::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_ANATOMY_PROVIDER_MANIFEST " << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema) << '\n';
    output << "PROVIDER " << std::quoted(provider) << '\n';
    output << "PUBLICATION_REFERENCE " << std::quoted(publication_reference) << '\n';
    output << "STATUS " << std::quoted(status) << '\n';
    output << "SOURCE_MODEL_REVISION " << (source_model_revision.has_value() ? 1U : 0U);
    if (source_model_revision.has_value()) output << ' ' << source_model_revision->value();
    output << '\n';
    output << "SOURCE_MANIFEST_DIGEST";
    write_optional_digest(output, source_manifest_digest);
    output << '\n';
    output << "PROVENANCE ";
    write_provenance(output, provenance);
    output << '\n';
    output << "BINDINGS " << bindings_.size() << '\n';
    for (const auto& [id, binding] : bindings_) {
        output << "BINDING " << id.value << ' ' << binding.part.value << ' '
               << std::quoted(binding.semantic_id) << ' ' << std::quoted(binding.laterality) << ' '
               << std::quoted(binding.source_geometry.kind) << ' '
               << std::quoted(binding.source_geometry.reference) << ' '
               << std::quoted(binding.source_release) << ' ' << std::quoted(binding.license);
        write_optional_digest(output, binding.geometry_digest);
        output << ' ' << std::quoted(binding.tissue_material) << ' '
               << std::quoted(binding.local_edit_reference) << ' ';
        write_provenance(output, binding.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<AnatomyProviderManifest> AnatomyProviderManifest::deserialize(
    std::string_view text) {
    if (text.size() > 8U * 1024U * 1024U) {
        return core::Result<AnatomyProviderManifest>::failure(
            parse_error("anatomy provider manifest is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_ANATOMY_PROVIDER_MANIFEST"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto version = read_uint(input, "anatomy provider manifest schema version");
    if (!version || (version.value() != 1U && version.value() != kSchemaVersion)) {
        return core::Result<AnatomyProviderManifest>::failure(Diagnostic(
            ErrorCode::version_mismatch, "unsupported anatomy provider manifest schema version"));
    }
    if (auto result = require_record(input, "SCHEMA"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto schema = read_string(input, "anatomy provider manifest schema", 128U, true);
    if (auto result = require_record(input, "PROVIDER"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto provider = read_string(input, "anatomy provider manifest provider", 256U, true);
    if (auto result = require_record(input, "PUBLICATION_REFERENCE"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto publication = read_string(input, "anatomy provider publication reference",
                                         kMaxReferenceBytes, true);
    if (auto result = require_record(input, "STATUS"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto status = read_string(input, "anatomy provider publication status", 128U, true);
    if (!schema || !provider || !publication || !status) {
        const auto& error = !schema ? schema.error() : !provider ? provider.error() :
            !publication ? publication.error() : status.error();
        return core::Result<AnatomyProviderManifest>::failure(error);
    }
    std::optional<core::Revision> source_model_revision;
    if (version.value() >= 2U) {
        if (auto result = require_record(input, "SOURCE_MODEL_REVISION"); !result) {
            return core::Result<AnatomyProviderManifest>::failure(result.error());
        }
        const auto present = read_uint(input, "anatomy provider source model revision presence");
        if (!present || present.value() > 1U) {
            return core::Result<AnatomyProviderManifest>::failure(parse_error(
                "invalid anatomy provider source model revision presence"));
        }
        if (present.value() != 0U) {
            const auto revision = read_uint(input, "anatomy provider source model revision");
            if (!revision || revision.value() == 0U) {
                return core::Result<AnatomyProviderManifest>::failure(parse_error(
                    "invalid anatomy provider source model revision"));
            }
            source_model_revision = core::Revision{revision.value()};
        }
    }
    if (auto result = require_record(input, "SOURCE_MANIFEST_DIGEST"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto source_digest = read_optional_digest(input, "anatomy provider source manifest");
    if (!source_digest) return core::Result<AnatomyProviderManifest>::failure(source_digest.error());
    if (auto result = require_record(input, "PROVENANCE"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto provenance = read_provenance(input);
    if (!provenance) return core::Result<AnatomyProviderManifest>::failure(provenance.error());

    AnatomyProviderManifest manifest;
    manifest.schema = schema.value();
    manifest.provider = provider.value();
    manifest.publication_reference = publication.value();
    manifest.status = status.value();
    manifest.source_model_revision = source_model_revision;
    manifest.source_manifest_digest = source_digest.value();
    manifest.provenance = provenance.value();
    if (auto result = require_record(input, "BINDINGS"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    const auto count = read_uint(input, "anatomy provider binding count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<AnatomyProviderManifest>::failure(parse_error(
            "invalid anatomy provider binding count"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "BINDING"); !result) {
            return core::Result<AnatomyProviderManifest>::failure(result.error());
        }
        const auto id = read_uint(input, "anatomy provider binding id");
        const auto part = read_uint(input, "anatomy provider binding part");
        const auto semantic = read_string(input, "anatomy provider semantic id", 256U, true);
        const auto laterality = read_string(input, "anatomy provider laterality", 128U, true);
        const auto geometry_kind = read_string(input, "anatomy provider geometry kind", 128U, true);
        const auto geometry_reference = read_string(input, "anatomy provider geometry reference",
                                                     kMaxReferenceBytes, true);
        const auto release = read_string(input, "anatomy provider source release", 256U, true);
        const auto license = read_string(input, "anatomy provider license", 256U, true);
        const auto geometry_digest = read_optional_digest(input, "anatomy provider geometry");
        const auto tissue = read_string(input, "anatomy provider tissue material", 256U);
        const auto local_edit = read_string(input, "anatomy provider local edit reference",
                                             kMaxReferenceBytes);
        const auto binding_provenance = read_provenance(input);
        if (!id || !part || !semantic || !laterality || !geometry_kind || !geometry_reference ||
            !release || !license || !geometry_digest || !tissue || !local_edit ||
            !binding_provenance) {
            const auto& error = !id ? id.error() : !part ? part.error() : !semantic ? semantic.error() :
                !laterality ? laterality.error() : !geometry_kind ? geometry_kind.error() :
                !geometry_reference ? geometry_reference.error() : !release ? release.error() :
                !license ? license.error() : !geometry_digest ? geometry_digest.error() :
                !tissue ? tissue.error() : !local_edit ? local_edit.error() :
                binding_provenance.error();
            return core::Result<AnatomyProviderManifest>::failure(error);
        }
        const auto inserted = manifest.insert_binding(AnatomyProviderBinding{
            AnatomyBindingId{id.value()}, AnatomyPartId{part.value()}, semantic.value(),
            laterality.value(), GeometryBinding{geometry_kind.value(), geometry_reference.value()},
            release.value(), license.value(), geometry_digest.value(), tissue.value(),
            local_edit.value(), binding_provenance.value()});
        if (!inserted) return core::Result<AnatomyProviderManifest>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<AnatomyProviderManifest>::failure(
            parse_error("anatomy provider manifest contains trailing data"));
    }
    if (auto result = manifest.validate(); !result) {
        return core::Result<AnatomyProviderManifest>::failure(result.error());
    }
    return core::Result<AnatomyProviderManifest>::success(std::move(manifest));
}

namespace {

assets::Sha256Digest anatomy_provider_manifest_digest(const AnatomyProviderManifest& manifest) {
    const std::string encoded = manifest.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()});
}

} // namespace

core::Result<AnatomyProviderCandidate> AnatomyProviderCandidate::from_manifest(
    AnatomyProviderManifest manifest,
    core::Revision expected_source_revision) {
    if (auto result = manifest.validate_handoff_candidate(expected_source_revision); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error().with_context(
            "anatomy provider candidate"));
    }
    AnatomyProviderCandidate candidate{
        std::move(manifest), expected_source_revision, assets::Sha256Digest{}};
    candidate.manifest_digest = anatomy_provider_manifest_digest(candidate.manifest);
    if (auto result = candidate.validate(); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    return core::Result<AnatomyProviderCandidate>::success(std::move(candidate));
}

core::Result<void> AnatomyProviderCandidate::validate() const {
    if (auto result = manifest.validate_handoff_candidate(source_model_revision); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "anatomy provider candidate manifest"));
    }
    if (manifest_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate manifest digest must be non-zero"));
    }
    if (anatomy_provider_manifest_digest(manifest) != manifest_digest) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate manifest digest does not match its canonical manifest"));
    }
    return core::Result<void>::success();
}

std::string AnatomyProviderCandidate::serialize() const {
    if (auto result = validate(); !result) return {};
    const std::string encoded_manifest = manifest.serialize();
    if (encoded_manifest.empty()) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_ANATOMY_PROVIDER_CANDIDATE " << kSchemaVersion << '\n';
    output << "SOURCE_MODEL_REVISION " << source_model_revision.value() << '\n';
    output << "MANIFEST_DIGEST " << manifest_digest.hex() << '\n';
    output << "MANIFEST " << std::quoted(encoded_manifest) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<AnatomyProviderCandidate> AnatomyProviderCandidate::deserialize(
    std::string_view text) {
    constexpr std::size_t kMaxCandidateBytes = 8U * 1024U * 1024U;
    constexpr std::size_t kMaxManifestBytes = 4U * 1024U * 1024U;
    if (text.size() > kMaxCandidateBytes) {
        return core::Result<AnatomyProviderCandidate>::failure(parse_error(
            "anatomy provider candidate is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_ANATOMY_PROVIDER_CANDIDATE"); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    const auto version = read_uint(input, "anatomy provider candidate schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<AnatomyProviderCandidate>::failure(Diagnostic(
            ErrorCode::version_mismatch,
            "unsupported anatomy provider candidate schema version"));
    }
    if (auto result = require_record(input, "SOURCE_MODEL_REVISION"); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    const auto source_revision = read_uint(input, "anatomy provider candidate source revision");
    if (!source_revision || source_revision.value() == 0U) {
        return core::Result<AnatomyProviderCandidate>::failure(parse_error(
            "anatomy provider candidate source revision is invalid"));
    }
    if (auto result = require_record(input, "MANIFEST_DIGEST"); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    std::string encoded_digest;
    if (!(input >> encoded_digest)) {
        return core::Result<AnatomyProviderCandidate>::failure(parse_error(
            "anatomy provider candidate manifest digest is missing"));
    }
    const auto manifest_digest = assets::Sha256Digest::from_hex(encoded_digest);
    if (!manifest_digest || manifest_digest.value().is_zero()) {
        return core::Result<AnatomyProviderCandidate>::failure(parse_error(
            "anatomy provider candidate manifest digest is invalid"));
    }
    if (auto result = require_record(input, "MANIFEST"); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    std::string encoded_manifest;
    if (!(input >> std::quoted(encoded_manifest)) || encoded_manifest.size() > kMaxManifestBytes) {
        return core::Result<AnatomyProviderCandidate>::failure(parse_error(
            "anatomy provider candidate manifest is invalid or too large"));
    }
    const auto manifest = AnatomyProviderManifest::deserialize(encoded_manifest);
    if (!manifest) {
        return core::Result<AnatomyProviderCandidate>::failure(
            manifest.error().with_context("anatomy provider candidate manifest"));
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<AnatomyProviderCandidate>::failure(parse_error(
            "anatomy provider candidate contains trailing data"));
    }
    AnatomyProviderCandidate candidate{
        manifest.value(), core::Revision{source_revision.value()}, manifest_digest.value()};
    if (auto result = candidate.validate(); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    return core::Result<AnatomyProviderCandidate>::success(std::move(candidate));
}

core::Result<void> ScientificReplayCapsule::validate() const {
    if (auto result = validate_id(id.value, "scientific replay capsule id"); !result) return result;
    if (source_model_revision.value() == 0U) {
        return core::Result<void>::failure(validation(
            "scientific replay capsule source revision must be non-zero"));
    }
    if (auto result = validate_text(provider, "scientific replay provider", 256U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(solver_version, "scientific replay solver version",
                                    256U, true); !result) {
        return result;
    }
    if (seeds.size() > kMaxTags) {
        return core::Result<void>::failure(validation(
            "scientific replay seed list exceeds the safety limit"));
    }
    std::set<std::uint64_t> unique_seeds;
    for (const auto seed : seeds) {
        if (!unique_seeds.insert(seed).second) {
            return core::Result<void>::failure(validation(
                "scientific replay seed list contains a duplicate"));
        }
    }
    if (auto result = validate_id_list(parameters, "scientific replay parameters"); !result) {
        return result;
    }
    const auto validate_digests = [](const std::vector<assets::Sha256Digest>& digests,
                                     std::string_view field) -> core::Result<void> {
        if (digests.size() > kMaxTags) {
            return core::Result<void>::failure(validation(
                std::string(field) + " exceeds the digest safety limit"));
        }
        std::set<std::string> unique;
        for (const auto& digest : digests) {
            if (digest.is_zero()) {
                return core::Result<void>::failure(validation(
                    std::string(field) + " contains a zero digest"));
            }
            if (!unique.insert(digest.hex()).second) {
                return core::Result<void>::failure(validation(
                    std::string(field) + " contains a duplicate digest"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_digests(external_dataset_digests,
                                       "scientific replay external dataset digests"); !result) {
        return result;
    }
    if (auto result = validate_digests(expected_output_digests,
                                       "scientific replay expected output digests"); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "scientific replay capsule provenance must be explicit"));
    }
    return provenance.validate();
}

core::Result<void> ScientificReplayCapsuleModel::insert_capsule(ScientificReplayCapsule value) {
    return insert_unique(capsules_, std::move(value), "scientific replay capsule");
}

core::Result<void> ScientificReplayCapsuleModel::validate() const {
    if (auto result = validate_text(schema, "scientific replay schema", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(reference, "scientific replay reference",
                                    kMaxReferenceBytes, true); !result) {
        return result;
    }
    if (auto result = validate_text(status, "scientific replay status", 128U, true); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "scientific replay model provenance must be explicit"));
    }
    if (auto result = provenance.validate(); !result) return result;
    if (capsules_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "scientific replay capsule collection exceeds the safety limit"));
    }
    for (const auto& [id, capsule] : capsules_) {
        static_cast<void>(id);
        if (auto result = capsule.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("scientific replay capsule"));
        }
    }
    return core::Result<void>::success();
}

std::string ScientificReplayCapsuleModel::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_SCIENTIFIC_REPLAY_CAPSULES " << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema) << '\n';
    output << "REFERENCE " << std::quoted(reference) << '\n';
    output << "STATUS " << std::quoted(status) << '\n';
    output << "PROVENANCE ";
    write_provenance(output, provenance);
    output << '\n';
    output << "CAPSULES " << capsules_.size() << '\n';
    for (const auto& [id, capsule] : capsules_) {
        output << "CAPSULE " << id.value << ' ' << capsule.source_model_revision.value() << ' '
               << std::quoted(capsule.provider) << ' ' << std::quoted(capsule.solver_version) << ' '
               << capsule.seeds.size();
        for (const auto seed : capsule.seeds) output << ' ' << seed;
        output << ' ';
        write_id_list(output, capsule.parameters);
        output << ' ' << capsule.external_dataset_digests.size();
        for (const auto& digest : capsule.external_dataset_digests) {
            output << ' ' << digest.hex();
        }
        output << ' ' << capsule.expected_output_digests.size();
        for (const auto& digest : capsule.expected_output_digests) {
            output << ' ' << digest.hex();
        }
        output << ' ';
        write_provenance(output, capsule.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ScientificReplayCapsuleModel> ScientificReplayCapsuleModel::deserialize(
    std::string_view text) {
    if (text.size() > 8U * 1024U * 1024U) {
        return core::Result<ScientificReplayCapsuleModel>::failure(
            parse_error("scientific replay capsule model is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_SCIENTIFIC_REPLAY_CAPSULES"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    const auto version = read_uint(input, "scientific replay schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<ScientificReplayCapsuleModel>::failure(Diagnostic(
            ErrorCode::version_mismatch, "unsupported scientific replay schema version"));
    }
    if (auto result = require_record(input, "SCHEMA"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    const auto schema = read_string(input, "scientific replay schema", 128U, true);
    if (auto result = require_record(input, "REFERENCE"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    const auto reference = read_string(input, "scientific replay reference",
                                       kMaxReferenceBytes, true);
    if (auto result = require_record(input, "STATUS"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    const auto status = read_string(input, "scientific replay status", 128U, true);
    if (!schema || !reference || !status) {
        const auto& error = !schema ? schema.error() : !reference ? reference.error() : status.error();
        return core::Result<ScientificReplayCapsuleModel>::failure(error);
    }
    if (auto result = require_record(input, "PROVENANCE"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    const auto provenance = read_provenance(input);
    if (!provenance) return core::Result<ScientificReplayCapsuleModel>::failure(provenance.error());
    ScientificReplayCapsuleModel model;
    model.schema = schema.value();
    model.reference = reference.value();
    model.status = status.value();
    model.provenance = provenance.value();
    if (auto result = require_record(input, "CAPSULES"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    const auto count = read_uint(input, "scientific replay capsule count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<ScientificReplayCapsuleModel>::failure(parse_error(
            "invalid scientific replay capsule count"));
    }
    const auto read_digest_list = [&input](std::string_view field) {
        const auto count = read_uint(input, std::string(field) + " count");
        if (!count || count.value() > kMaxTags) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(parse_error(
                std::string(field) + " count is invalid"));
        }
        std::vector<assets::Sha256Digest> digests;
        digests.reserve(static_cast<std::size_t>(count.value()));
        for (std::uint64_t index = 0U; index < count.value(); ++index) {
            std::string encoded;
            if (!(input >> encoded)) {
                return core::Result<std::vector<assets::Sha256Digest>>::failure(parse_error(
                    std::string(field) + " digest is missing"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded);
            if (!digest || digest.value().is_zero()) {
                return core::Result<std::vector<assets::Sha256Digest>>::failure(parse_error(
                    std::string(field) + " digest is invalid"));
            }
            digests.push_back(digest.value());
        }
        return core::Result<std::vector<assets::Sha256Digest>>::success(std::move(digests));
    };
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "CAPSULE"); !result) {
            return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
        }
        const auto id = read_uint(input, "scientific replay capsule id");
        const auto revision = read_uint(input, "scientific replay source revision");
        const auto provider = read_string(input, "scientific replay provider", 256U, true);
        const auto solver = read_string(input, "scientific replay solver version", 256U, true);
        const auto seed_count = read_uint(input, "scientific replay seed count");
        if (!id || !revision || !provider || !solver || !seed_count ||
            seed_count.value() > kMaxTags) {
            return core::Result<ScientificReplayCapsuleModel>::failure(parse_error(
                "invalid scientific replay capsule header"));
        }
        std::vector<std::uint64_t> seeds;
        seeds.reserve(static_cast<std::size_t>(seed_count.value()));
        for (std::uint64_t seed = 0U; seed < seed_count.value(); ++seed) {
            const auto value = read_uint(input, "scientific replay seed");
            if (!value) return core::Result<ScientificReplayCapsuleModel>::failure(value.error());
            seeds.push_back(value.value());
        }
        const auto parameters = read_id_list<ParameterId>(input, "scientific replay parameters");
        const auto external_digests = read_digest_list("scientific replay external dataset digests");
        const auto expected_digests = read_digest_list("scientific replay expected output digests");
        const auto capsule_provenance = read_provenance(input);
        if (!parameters || !external_digests || !expected_digests || !capsule_provenance) {
            const auto& error = !parameters ? parameters.error() :
                !external_digests ? external_digests.error() :
                !expected_digests ? expected_digests.error() : capsule_provenance.error();
            return core::Result<ScientificReplayCapsuleModel>::failure(error);
        }
        const auto inserted = model.insert_capsule(ScientificReplayCapsule{
            ReplayId{id.value()}, core::Revision{revision.value()}, provider.value(), solver.value(),
            std::move(seeds), parameters.value(), external_digests.value(), expected_digests.value(),
            capsule_provenance.value()});
        if (!inserted) return core::Result<ScientificReplayCapsuleModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ScientificReplayCapsuleModel>::failure(
            parse_error("scientific replay capsule model contains trailing data"));
    }
    if (auto result = model.validate(); !result) {
        return core::Result<ScientificReplayCapsuleModel>::failure(result.error());
    }
    return core::Result<ScientificReplayCapsuleModel>::success(std::move(model));
}

namespace {

bool supported_preview_palette(std::string_view palette) {
    return palette == "gray" || palette == "grayscale" || palette == "viridis";
}

core::Result<std::array<double, 4>> preview_color(
    std::string_view palette,
    double normalized) {
    if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0) {
        return core::Result<std::array<double, 4>>::failure(validation(
            "field visualization normalized value is outside [0,1]"));
    }
    if (palette == "gray" || palette == "grayscale") {
        return core::Result<std::array<double, 4>>::success(
            {normalized, normalized, normalized, 1.0});
    }
    if (palette != "viridis") {
        return core::Result<std::array<double, 4>>::failure(validation(
            "field visualization preview palette is unsupported"));
    }

    constexpr std::array<std::array<double, 3>, 3> kViridisStops{{
        {0.267004, 0.004874, 0.329415},
        {0.127568, 0.566949, 0.550556},
        {0.993248, 0.906157, 0.143936},
    }};
    const double scaled = normalized * 2.0;
    const std::size_t segment = scaled >= 2.0 ? 1U : static_cast<std::size_t>(scaled);
    const double fraction = scaled - static_cast<double>(segment);
    const auto& first = kViridisStops[segment];
    const auto& second = kViridisStops[segment + 1U];
    return core::Result<std::array<double, 4>>::success({
        first[0] + (second[0] - first[0]) * fraction,
        first[1] + (second[1] - first[1]) * fraction,
        first[2] + (second[2] - first[2]) * fraction,
        1.0,
    });
}

core::Result<std::size_t> preview_grid_sample_count(
    std::uint32_t width,
    std::uint32_t height,
    std::string_view label) {
    if (width < 2U || height < 2U) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " grid dimensions must both be at least two"));
    }
    const auto width_size = static_cast<std::size_t>(width);
    const auto height_size = static_cast<std::size_t>(height);
    if (width_size > kMaxCollectionEntries / height_size) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " grid exceeds the safety limit"));
    }
    const auto sample_count = width_size * height_size;
    if (sample_count > kMaxCollectionEntries) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " grid exceeds the safety limit"));
    }
    return core::Result<std::size_t>::success(sample_count);
}

core::Result<std::size_t> preview_volume_sample_count(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    std::string_view label) {
    if (width < 2U || height < 2U || depth < 2U) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " volume dimensions must all be at least two"));
    }
    const auto width_size = static_cast<std::size_t>(width);
    const auto height_size = static_cast<std::size_t>(height);
    const auto depth_size = static_cast<std::size_t>(depth);
    if (width_size > kMaxCollectionEntries / height_size) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " volume exceeds the safety limit"));
    }
    const auto plane_sample_count = width_size * height_size;
    if (plane_sample_count > kMaxCollectionEntries / depth_size) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " volume exceeds the safety limit"));
    }
    const auto sample_count = plane_sample_count * depth_size;
    if (sample_count > kMaxCollectionEntries) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " volume exceeds the safety limit"));
    }
    return core::Result<std::size_t>::success(sample_count);
}

core::Result<std::size_t> preview_volume_ray_sample_count(
    std::uint32_t volume_width,
    std::uint32_t volume_height,
    std::uint32_t volume_depth,
    std::uint32_t output_width,
    std::uint32_t output_height,
    std::string_view label) {
    const auto volume_samples = preview_volume_sample_count(
        volume_width, volume_height, volume_depth, label);
    if (!volume_samples) return core::Result<std::size_t>::failure(volume_samples.error());
    const auto output_samples = preview_grid_sample_count(output_width, output_height, label);
    if (!output_samples) return core::Result<std::size_t>::failure(output_samples.error());
    if (output_samples.value() > kMaxVolumeRaySamples / volume_depth) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " ray work exceeds the safety limit"));
    }
    const auto ray_samples = output_samples.value() * static_cast<std::size_t>(volume_depth);
    if (ray_samples > kMaxVolumeRaySamples) {
        return core::Result<std::size_t>::failure(validation(
            std::string(label) + " ray work exceeds the safety limit"));
    }
    return core::Result<std::size_t>::success(ray_samples);
}

core::Result<void> validate_slice_frame(
    core::Vec3d origin,
    core::Vec3d normal,
    core::Vec3d horizontal_axis,
    core::Vec3d vertical_axis,
    std::string_view label) {
    if (!origin.finite() || !normal.finite() || !horizontal_axis.finite() ||
        !vertical_axis.finite()) {
        return core::Result<void>::failure(validation(
            std::string(label) + " frame contains a non-finite vector"));
    }
    constexpr double kFrameTolerance = 1.0e-9;
    const auto close_to_one = [](double value) {
        return std::abs(value - 1.0) <= 1.0e-9;
    };
    if (!close_to_one(normal.length()) || !close_to_one(horizontal_axis.length()) ||
        !close_to_one(vertical_axis.length()) ||
        std::abs(core::dot(normal, horizontal_axis)) > kFrameTolerance ||
        std::abs(core::dot(normal, vertical_axis)) > kFrameTolerance ||
        std::abs(core::dot(horizontal_axis, vertical_axis)) > kFrameTolerance ||
        core::dot(core::cross(horizontal_axis, vertical_axis), normal) <
            1.0 - kFrameTolerance) {
        return core::Result<void>::failure(validation(
            std::string(label) + " frame must be right-handed and orthonormal"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> FieldVisualizationPreview::validate() const {
    if (auto result = validate_id(visualization.value, "field visualization preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field visualization preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field visualization preview mode", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field visualization preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field visualization preview supports only scalar gray/grayscale/viridis data"));
    }
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound) {
        return core::Result<void>::failure(validation(
            "field visualization preview bounds are invalid"));
    }
    if (samples.empty() || samples.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "field visualization preview samples are empty or exceed the safety limit"));
    }
    const auto sample_count = static_cast<std::uint64_t>(samples.size());
    if (clipped_low > sample_count || clipped_high > sample_count - clipped_low) {
        return core::Result<void>::failure(validation(
            "field visualization preview clipping counts exceed the sample count"));
    }
    if (legend_ticks.empty() || legend_ticks.size() > 64U) {
        return core::Result<void>::failure(validation(
            "field visualization preview legend is empty or exceeds the safety limit"));
    }
    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field visualization preview range is not finite"));
    }
    std::uint64_t expected_clipped_low = 0U;
    std::uint64_t expected_clipped_high = 0U;
    for (const auto& sample : samples) {
        if (!std::isfinite(sample.source_value) || !std::isfinite(sample.normalized_value) ||
            sample.normalized_value < 0.0 || sample.normalized_value > 1.0) {
            return core::Result<void>::failure(validation(
                "field visualization preview sample contains a non-finite or unnormalized value"));
        }
        double expected_normalized = 0.5;
        if (range == 0.0) {
            if (sample.source_value != lower_bound) {
                return core::Result<void>::failure(validation(
                    "field visualization preview zero-width range does not contain every sample"));
            }
        } else if (sample.source_value < lower_bound) {
            expected_normalized = 0.0;
            ++expected_clipped_low;
        } else if (sample.source_value > upper_bound) {
            expected_normalized = 1.0;
            ++expected_clipped_high;
        } else {
            expected_normalized = (sample.source_value - lower_bound) / range;
        }
        if (std::abs(sample.normalized_value - expected_normalized) > 1e-12) {
            return core::Result<void>::failure(validation(
                "field visualization preview normalized value does not match its source sample"));
        }
        const auto expected_color = preview_color(palette, expected_normalized);
        if (!expected_color) return core::Result<void>::failure(expected_color.error());
        for (const double channel : sample.rgba) {
            if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0) {
                return core::Result<void>::failure(validation(
                    "field visualization preview sample contains an invalid color"));
            }
        }
        for (std::size_t channel = 0U; channel < sample.rgba.size(); ++channel) {
            if (std::abs(sample.rgba[channel] - expected_color.value()[channel]) > 1e-12) {
                return core::Result<void>::failure(validation(
                    "field visualization preview color does not match its palette"));
            }
        }
    }
    if (clipped_low != expected_clipped_low || clipped_high != expected_clipped_high) {
        return core::Result<void>::failure(validation(
            "field visualization preview clipping counts do not match its samples"));
    }
    for (std::size_t index = 0U; index < legend_ticks.size(); ++index) {
        const double tick = legend_ticks[index];
        if (!std::isfinite(tick) || tick < lower_bound || tick > upper_bound ||
            (index != 0U && tick < legend_ticks[index - 1U])) {
            return core::Result<void>::failure(validation(
                "field visualization preview legend is not finite, bounded, and ordered"));
        }
    }
    return core::Result<void>::success();
}

core::Result<FieldVisualizationPreview> FieldVisualization::preview_samples(
    std::span<const double> values) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldVisualizationPreview>::failure(
            result.error().with_context("field visualization preview"));
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<FieldVisualizationPreview>::failure(validation(
            "field visualization preview supports only scalar gray/grayscale/viridis data"));
    }
    if (values.empty() || values.size() > kMaxCollectionEntries) {
        return core::Result<FieldVisualizationPreview>::failure(validation(
            "field visualization preview input is empty or exceeds the safety limit"));
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<FieldVisualizationPreview>::failure(validation(
                "field visualization preview input contains a non-finite sample"));
        }
        observed_lower = std::min(observed_lower, value);
        observed_upper = std::max(observed_upper, value);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldVisualizationPreview>::failure(validation(
            "field visualization preview range is invalid"));
    }

    FieldVisualizationPreview preview{
        id, field, mode, palette, lower, upper, 0U, 0U, {}, {}};
    preview.samples.reserve(values.size());
    preview.legend_ticks.reserve(5U);
    const double range = upper - lower;
    for (std::size_t index = 0U; index < 5U; ++index) {
        const double fraction = static_cast<double>(index) / 4.0;
        preview.legend_ticks.push_back(lower + range * fraction);
    }

    for (const double value : values) {
        double normalized = 0.5;
        if (range == 0.0) {
            if (value != lower) {
                return core::Result<FieldVisualizationPreview>::failure(validation(
                    "field visualization preview zero-width range does not contain every sample"));
            }
        } else if (value < lower) {
            normalized = 0.0;
            ++preview.clipped_low;
        } else if (value > upper) {
            normalized = 1.0;
            ++preview.clipped_high;
        } else {
            normalized = (value - lower) / range;
        }
        const auto color = preview_color(palette, normalized);
        if (!color) return core::Result<FieldVisualizationPreview>::failure(color.error());
        preview.samples.push_back(FieldVisualizationSample{value, normalized, color.value()});
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldVisualizationPreview>::failure(result.error());
    }
    return core::Result<FieldVisualizationPreview>::success(std::move(preview));
}

core::Result<void> FieldSlicePreview::validate() const {
    if (auto result = validate_id(visualization.value, "field slice preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field slice preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field slice preview mode", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field slice preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "slice" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field slice preview supports only slice gray/grayscale/viridis data"));
    }
    const auto sample_count = preview_grid_sample_count(width, height, "field slice preview");
    if (!sample_count) return core::Result<void>::failure(sample_count.error());
    if (samples.size() != sample_count.value()) {
        return core::Result<void>::failure(validation(
            "field slice preview sample count does not match its grid"));
    }
    if (auto result = validate_slice_frame(
            origin, normal, horizontal_axis, vertical_axis, "field slice preview");
        !result) {
        return result;
    }
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound) {
        return core::Result<void>::failure(validation(
            "field slice preview bounds are invalid"));
    }
    const auto count = static_cast<std::uint64_t>(samples.size());
    if (clipped_low > count || clipped_high > count - clipped_low) {
        return core::Result<void>::failure(validation(
            "field slice preview clipping counts exceed the sample count"));
    }
    if (legend_ticks.empty() || legend_ticks.size() > 64U) {
        return core::Result<void>::failure(validation(
            "field slice preview legend is empty or exceeds the safety limit"));
    }
    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field slice preview range is not finite"));
    }
    std::uint64_t expected_clipped_low = 0U;
    std::uint64_t expected_clipped_high = 0U;
    for (const auto& sample : samples) {
        if (!std::isfinite(sample.source_value) || !std::isfinite(sample.normalized_value) ||
            sample.normalized_value < 0.0 || sample.normalized_value > 1.0) {
            return core::Result<void>::failure(validation(
                "field slice preview sample contains a non-finite or unnormalized value"));
        }
        double expected_normalized = 0.5;
        if (range == 0.0) {
            if (sample.source_value != lower_bound) {
                return core::Result<void>::failure(validation(
                    "field slice preview zero-width range does not contain every sample"));
            }
        } else if (sample.source_value < lower_bound) {
            expected_normalized = 0.0;
            ++expected_clipped_low;
        } else if (sample.source_value > upper_bound) {
            expected_normalized = 1.0;
            ++expected_clipped_high;
        } else {
            expected_normalized = (sample.source_value - lower_bound) / range;
        }
        if (std::abs(sample.normalized_value - expected_normalized) > 1.0e-12) {
            return core::Result<void>::failure(validation(
                "field slice preview normalized value does not match its source sample"));
        }
        const auto expected_color = preview_color(palette, expected_normalized);
        if (!expected_color) return core::Result<void>::failure(expected_color.error());
        for (std::size_t channel = 0U; channel < sample.rgba.size(); ++channel) {
            if (!std::isfinite(sample.rgba[channel]) || sample.rgba[channel] < 0.0 ||
                sample.rgba[channel] > 1.0 ||
                std::abs(sample.rgba[channel] - expected_color.value()[channel]) > 1.0e-12) {
                return core::Result<void>::failure(validation(
                    "field slice preview color does not match its palette"));
            }
        }
    }
    if (clipped_low != expected_clipped_low || clipped_high != expected_clipped_high) {
        return core::Result<void>::failure(validation(
            "field slice preview clipping counts do not match its samples"));
    }
    for (std::size_t index = 0U; index < legend_ticks.size(); ++index) {
        const double tick = legend_ticks[index];
        if (!std::isfinite(tick) || tick < lower_bound || tick > upper_bound ||
            (index != 0U && tick < legend_ticks[index - 1U])) {
            return core::Result<void>::failure(validation(
                "field slice preview legend is not finite, bounded, and ordered"));
        }
    }
    return core::Result<void>::success();
}

core::Result<FieldSlicePreview> FieldVisualization::preview_slice(
    std::span<const double> values,
    std::uint32_t width,
    std::uint32_t height,
    core::Vec3d origin,
    core::Vec3d normal,
    core::Vec3d horizontal_axis,
    core::Vec3d vertical_axis) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldSlicePreview>::failure(
            result.error().with_context("field slice preview"));
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<FieldSlicePreview>::failure(validation(
            "field slice preview requires a scalar gray/grayscale/viridis visualization"));
    }
    const auto sample_count = preview_grid_sample_count(width, height, "field slice preview");
    if (!sample_count) return core::Result<FieldSlicePreview>::failure(sample_count.error());
    if (values.size() != sample_count.value()) {
        return core::Result<FieldSlicePreview>::failure(validation(
            "field slice preview input count does not match its grid"));
    }
    if (auto result = validate_slice_frame(
            origin, normal, horizontal_axis, vertical_axis, "field slice preview");
        !result) {
        return core::Result<FieldSlicePreview>::failure(result.error());
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<FieldSlicePreview>::failure(validation(
                "field slice preview input contains a non-finite sample"));
        }
        observed_lower = std::min(observed_lower, value);
        observed_upper = std::max(observed_upper, value);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldSlicePreview>::failure(validation(
            "field slice preview range is invalid"));
    }

    FieldSlicePreview preview{
        id, field, "slice", palette, width, height, origin, normal, horizontal_axis,
        vertical_axis, lower, upper, 0U, 0U, {}, {}};
    preview.samples.reserve(values.size());
    preview.legend_ticks.reserve(5U);
    const double range = upper - lower;
    for (std::size_t index = 0U; index < 5U; ++index) {
        const double fraction = static_cast<double>(index) / 4.0;
        preview.legend_ticks.push_back(lower + range * fraction);
    }
    for (const double value : values) {
        double normalized = 0.5;
        if (range == 0.0) {
            if (value != lower) {
                return core::Result<FieldSlicePreview>::failure(validation(
                    "field slice preview zero-width range does not contain every sample"));
            }
        } else if (value < lower) {
            normalized = 0.0;
            ++preview.clipped_low;
        } else if (value > upper) {
            normalized = 1.0;
            ++preview.clipped_high;
        } else {
            normalized = (value - lower) / range;
        }
        const auto color = preview_color(palette, normalized);
        if (!color) return core::Result<FieldSlicePreview>::failure(color.error());
        preview.samples.push_back(FieldVisualizationSample{value, normalized, color.value()});
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldSlicePreview>::failure(result.error());
    }
    return core::Result<FieldSlicePreview>::success(std::move(preview));
}

core::Result<void> FieldContourPreview::validate() const {
    if (auto result = validate_id(visualization.value, "field contour preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field contour preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field contour preview mode", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field contour preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "contour" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field contour preview supports only contour gray/grayscale/viridis data"));
    }
    const auto sample_count = preview_grid_sample_count(width, height, "field contour preview");
    if (!sample_count) return core::Result<void>::failure(sample_count.error());
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound) {
        return core::Result<void>::failure(validation(
            "field contour preview bounds are invalid"));
    }
    if (levels.empty() || levels.size() > 256U) {
        return core::Result<void>::failure(validation(
            "field contour preview levels are empty or exceed the safety limit"));
    }
    for (std::size_t index = 0U; index < levels.size(); ++index) {
        if (!std::isfinite(levels[index]) || levels[index] < lower_bound ||
            levels[index] > upper_bound || (index != 0U && levels[index] <= levels[index - 1U])) {
            return core::Result<void>::failure(validation(
                "field contour preview levels are not finite, bounded, and strictly ordered"));
        }
    }
    if (segments.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "field contour preview segments exceed the safety limit"));
    }
    const double range = upper_bound - lower_bound;
    const auto close = [](double left, double right) {
        return std::abs(left - right) <=
            1.0e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    for (const auto& segment : segments) {
        if (!segment.start.finite() || !segment.end.finite() || !std::isfinite(segment.level) ||
            !std::isfinite(segment.rgba[0]) || !std::isfinite(segment.rgba[1]) ||
            !std::isfinite(segment.rgba[2]) || !std::isfinite(segment.rgba[3]) ||
            segment.start.x < 0.0 || segment.start.x > 1.0 || segment.start.y < 0.0 ||
            segment.start.y > 1.0 || segment.start.z != 0.0 || segment.end.x < 0.0 ||
            segment.end.x > 1.0 || segment.end.y < 0.0 || segment.end.y > 1.0 ||
            segment.end.z != 0.0 || (segment.start.x == segment.end.x &&
                segment.start.y == segment.end.y) || segment.level < lower_bound ||
            segment.level > upper_bound) {
            return core::Result<void>::failure(validation(
                "field contour preview contains an invalid segment"));
        }
        bool known_level = false;
        for (const double level : levels) {
            if (close(level, segment.level)) {
                known_level = true;
                break;
            }
        }
        if (!known_level) {
            return core::Result<void>::failure(validation(
                "field contour preview segment names an unknown level"));
        }
        double normalized = 0.5;
        if (range != 0.0) normalized = (segment.level - lower_bound) / range;
        const auto expected_color = preview_color(palette, normalized);
        if (!expected_color) return core::Result<void>::failure(expected_color.error());
        for (std::size_t channel = 0U; channel < segment.rgba.size(); ++channel) {
            if (segment.rgba[channel] < 0.0 || segment.rgba[channel] > 1.0 ||
                !close(segment.rgba[channel], expected_color.value()[channel])) {
                return core::Result<void>::failure(validation(
                    "field contour preview segment color does not match its level"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<FieldContourPreview> FieldVisualization::preview_contours(
    std::span<const double> values,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const double> levels) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldContourPreview>::failure(
            result.error().with_context("field contour preview"));
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<FieldContourPreview>::failure(validation(
            "field contour preview requires a scalar gray/grayscale/viridis visualization"));
    }
    const auto sample_count = preview_grid_sample_count(width, height, "field contour preview");
    if (!sample_count) return core::Result<FieldContourPreview>::failure(sample_count.error());
    if (values.size() != sample_count.value()) {
        return core::Result<FieldContourPreview>::failure(validation(
            "field contour preview input count does not match its grid"));
    }
    if (levels.empty() || levels.size() > 256U) {
        return core::Result<FieldContourPreview>::failure(validation(
            "field contour preview levels are empty or exceed the safety limit"));
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<FieldContourPreview>::failure(validation(
                "field contour preview input contains a non-finite sample"));
        }
        observed_lower = std::min(observed_lower, value);
        observed_upper = std::max(observed_upper, value);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldContourPreview>::failure(validation(
            "field contour preview range is invalid"));
    }
    for (std::size_t index = 0U; index < levels.size(); ++index) {
        if (!std::isfinite(levels[index]) || levels[index] < lower || levels[index] > upper ||
            (index != 0U && levels[index] <= levels[index - 1U])) {
            return core::Result<FieldContourPreview>::failure(validation(
                "field contour preview levels are not finite, bounded, and strictly ordered"));
        }
    }
    const auto cell_count = (static_cast<std::size_t>(width) - 1U) *
        (static_cast<std::size_t>(height) - 1U);
    if (levels.size() > kMaxCollectionEntries / 2U / std::max<std::size_t>(1U, cell_count)) {
        return core::Result<FieldContourPreview>::failure(validation(
            "field contour preview may produce too many segments"));
    }

    FieldContourPreview preview{
        id, field, "contour", palette, width, height, lower, upper,
        std::vector<double>(levels.begin(), levels.end()), {}};
    preview.segments.reserve(cell_count * levels.size());
    const auto position = [width, height](std::size_t x, std::size_t y) {
        return core::Vec3d{
            static_cast<double>(x) / static_cast<double>(width - 1U),
            static_cast<double>(y) / static_cast<double>(height - 1U),
            0.0};
    };
    constexpr std::array<std::array<std::size_t, 2>, 4> kEdges{{
        {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}}};
    const double range = upper - lower;
    for (std::size_t y = 0U; y + 1U < height; ++y) {
        for (std::size_t x = 0U; x + 1U < width; ++x) {
            const std::array<std::size_t, 4> indices{{
                y * width + x, y * width + x + 1U,
                (y + 1U) * width + x + 1U, (y + 1U) * width + x}};
            const std::array<core::Vec3d, 4> positions{{
                position(x, y), position(x + 1U, y), position(x + 1U, y + 1U),
                position(x, y + 1U)}};
            const std::array<double, 4> corner_values{{
                values[indices[0]], values[indices[1]], values[indices[2]], values[indices[3]]}};
            for (const double level : levels) {
                unsigned case_index = 0U;
                for (unsigned corner = 0U; corner < 4U; ++corner) {
                    if (corner_values[corner] > level) case_index |= 1U << corner;
                }
                const auto intersection = [&](int edge) {
                    const auto start = kEdges[static_cast<std::size_t>(edge)][0];
                    const auto end = kEdges[static_cast<std::size_t>(edge)][1];
                    const double denominator = corner_values[end] - corner_values[start];
                    const double fraction = std::clamp(
                        (level - corner_values[start]) / denominator, 0.0, 1.0);
                    return positions[start] + (positions[end] - positions[start]) * fraction;
                };
                const double center = (corner_values[0] + corner_values[1] +
                                       corner_values[2] + corner_values[3]) * 0.25;
                const auto emit = [&](int first_edge, int second_edge) {
                    const double normalized = range == 0.0 ? 0.5 : (level - lower) / range;
                    const auto color = preview_color(palette, normalized);
                    if (!color) return false;
                    preview.segments.push_back(FieldContourSegment{
                        intersection(first_edge), intersection(second_edge), level, color.value()});
                    return true;
                };
                bool emitted = true;
                switch (case_index) {
                case 1U: emitted = emit(3, 0); break;
                case 2U: emitted = emit(0, 1); break;
                case 3U: emitted = emit(3, 1); break;
                case 4U: emitted = emit(1, 2); break;
                case 5U:
                    if (center > level) {
                        emitted = emit(0, 1) && emit(2, 3);
                    } else {
                        emitted = emit(3, 0) && emit(1, 2);
                    }
                    break;
                case 6U: emitted = emit(0, 2); break;
                case 7U: emitted = emit(2, 3); break;
                case 8U: emitted = emit(2, 3); break;
                case 9U: emitted = emit(0, 2); break;
                case 10U:
                    if (center > level) {
                        emitted = emit(3, 0) && emit(1, 2);
                    } else {
                        emitted = emit(0, 1) && emit(2, 3);
                    }
                    break;
                case 11U: emitted = emit(1, 2); break;
                case 12U: emitted = emit(1, 3); break;
                case 13U: emitted = emit(0, 1); break;
                case 14U: emitted = emit(3, 0); break;
                default: break;
                }
                if (!emitted) {
                    return core::Result<FieldContourPreview>::failure(validation(
                        "field contour preview color projection failed"));
                }
            }
        }
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldContourPreview>::failure(result.error());
    }
    return core::Result<FieldContourPreview>::success(std::move(preview));
}

core::Result<void> FieldIsosurfacePreview::validate() const {
    if (auto result = validate_id(visualization.value, "field isosurface preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field isosurface preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field isosurface preview mode", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field isosurface preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "isosurface" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field isosurface preview supports only isosurface gray/grayscale/viridis data"));
    }
    const auto sample_count = preview_volume_sample_count(
        width, height, depth, "field isosurface preview");
    if (!sample_count) return core::Result<void>::failure(sample_count.error());
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound ||
        !std::isfinite(level) || level < lower_bound || level > upper_bound) {
        return core::Result<void>::failure(validation(
            "field isosurface preview bounds or level are invalid"));
    }
    if (triangles.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "field isosurface preview triangles exceed the safety limit"));
    }
    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field isosurface preview range is not finite"));
    }
    const double normalized = range == 0.0 ? 0.5 : (level - lower_bound) / range;
    const auto expected_color = preview_color(palette, normalized);
    if (!expected_color) return core::Result<void>::failure(expected_color.error());
    const auto close = [](double left, double right) {
        return std::abs(left - right) <=
            1.0e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    for (const auto& triangle : triangles) {
        if (!triangle.a.finite() || !triangle.b.finite() || !triangle.c.finite() ||
            triangle.a.x < 0.0 || triangle.a.x > 1.0 || triangle.a.y < 0.0 ||
            triangle.a.y > 1.0 || triangle.a.z < 0.0 || triangle.a.z > 1.0 ||
            triangle.b.x < 0.0 || triangle.b.x > 1.0 || triangle.b.y < 0.0 ||
            triangle.b.y > 1.0 || triangle.b.z < 0.0 || triangle.b.z > 1.0 ||
            triangle.c.x < 0.0 || triangle.c.x > 1.0 || triangle.c.y < 0.0 ||
            triangle.c.y > 1.0 || triangle.c.z < 0.0 || triangle.c.z > 1.0 ||
            !std::isfinite(triangle.level) || !close(triangle.level, level)) {
            return core::Result<void>::failure(validation(
                "field isosurface preview contains an invalid triangle"));
        }
        const auto cross_product = core::cross(triangle.b - triangle.a, triangle.c - triangle.a);
        if (!cross_product.finite() || cross_product.length() <= 1.0e-14) {
            return core::Result<void>::failure(validation(
                "field isosurface preview contains a degenerate triangle"));
        }
        for (std::size_t channel = 0U; channel < triangle.rgba.size(); ++channel) {
            if (!std::isfinite(triangle.rgba[channel]) || triangle.rgba[channel] < 0.0 ||
                triangle.rgba[channel] > 1.0 ||
                !close(triangle.rgba[channel], expected_color.value()[channel])) {
                return core::Result<void>::failure(validation(
                    "field isosurface preview triangle color does not match its level"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<FieldIsosurfacePreview> FieldVisualization::preview_isosurface(
    std::span<const double> values,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t depth,
    double level) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldIsosurfacePreview>::failure(
            result.error().with_context("field isosurface preview"));
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<FieldIsosurfacePreview>::failure(validation(
            "field isosurface preview requires a scalar gray/grayscale/viridis visualization"));
    }
    const auto sample_count = preview_volume_sample_count(
        width, height, depth, "field isosurface preview");
    if (!sample_count) return core::Result<FieldIsosurfacePreview>::failure(sample_count.error());
    if (values.size() != sample_count.value()) {
        return core::Result<FieldIsosurfacePreview>::failure(validation(
            "field isosurface preview input count does not match its volume"));
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<FieldIsosurfacePreview>::failure(validation(
                "field isosurface preview input contains a non-finite sample"));
        }
        observed_lower = std::min(observed_lower, value);
        observed_upper = std::max(observed_upper, value);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower) || !std::isfinite(level) || level < lower ||
        level > upper) {
        return core::Result<FieldIsosurfacePreview>::failure(validation(
            "field isosurface preview bounds or level are invalid"));
    }

    const double range = upper - lower;
    const double normalized = range == 0.0 ? 0.5 : (level - lower) / range;
    const auto color = preview_color(palette, normalized);
    if (!color) return core::Result<FieldIsosurfacePreview>::failure(color.error());

    FieldIsosurfacePreview preview{
        id, field, "isosurface", palette, width, height, depth, lower, upper, level, {}};
    const auto cell_count = (static_cast<std::size_t>(width) - 1U) *
        (static_cast<std::size_t>(height) - 1U) *
        (static_cast<std::size_t>(depth) - 1U);
    preview.triangles.reserve(std::min(kMaxCollectionEntries, cell_count * 12U));

    const auto position = [width, height, depth](std::size_t x, std::size_t y, std::size_t z) {
        return core::Vec3d{
            static_cast<double>(x) / static_cast<double>(width - 1U),
            static_cast<double>(y) / static_cast<double>(height - 1U),
            static_cast<double>(z) / static_cast<double>(depth - 1U)};
    };
    const auto sample_index = [width, height](std::size_t x, std::size_t y, std::size_t z) {
        return z * static_cast<std::size_t>(width) * static_cast<std::size_t>(height) +
            y * static_cast<std::size_t>(width) + x;
    };
    constexpr std::array<std::array<std::size_t, 4>, 6> kTetrahedra{{
        {{0U, 5U, 1U, 6U}},
        {{0U, 1U, 2U, 6U}},
        {{0U, 2U, 3U, 6U}},
        {{0U, 3U, 7U, 6U}},
        {{0U, 7U, 4U, 6U}},
        {{0U, 4U, 5U, 6U}},
    }};
    constexpr std::array<std::array<std::size_t, 2>, 6> kTetrahedronEdges{{
        {{0U, 1U}}, {{0U, 2U}}, {{0U, 3U}},
        {{1U, 2U}}, {{1U, 3U}}, {{2U, 3U}},
    }};

    struct Crossing {
        std::size_t first = 0U;
        std::size_t second = 0U;
        core::Vec3d point;
    };
    const auto append_triangle = [&](core::Vec3d a, core::Vec3d b, core::Vec3d c) {
        const auto cross_product = core::cross(b - a, c - a);
        if (!cross_product.finite()) {
            return core::Result<void>::failure(validation(
                "field isosurface preview triangle is not finite"));
        }
        if (cross_product.length() <= 1.0e-14) {
            return core::Result<void>::success();
        }
        if (preview.triangles.size() >= kMaxCollectionEntries) {
            return core::Result<void>::failure(validation(
                "field isosurface preview produced too many triangles"));
        }
        preview.triangles.push_back(FieldIsosurfaceTriangle{a, b, c, level, color.value()});
        return core::Result<void>::success();
    };

    for (std::size_t z = 0U; z + 1U < depth; ++z) {
        for (std::size_t y = 0U; y + 1U < height; ++y) {
            for (std::size_t x = 0U; x + 1U < width; ++x) {
                const std::array<std::size_t, 8> cube_indices{{
                    sample_index(x, y, z), sample_index(x + 1U, y, z),
                    sample_index(x + 1U, y + 1U, z), sample_index(x, y + 1U, z),
                    sample_index(x, y, z + 1U), sample_index(x + 1U, y, z + 1U),
                    sample_index(x + 1U, y + 1U, z + 1U), sample_index(x, y + 1U, z + 1U)}};
                const std::array<core::Vec3d, 8> cube_positions{{
                    position(x, y, z), position(x + 1U, y, z),
                    position(x + 1U, y + 1U, z), position(x, y + 1U, z),
                    position(x, y, z + 1U), position(x + 1U, y, z + 1U),
                    position(x + 1U, y + 1U, z + 1U), position(x, y + 1U, z + 1U)}};
                std::array<double, 8> cube_values{};
                for (std::size_t corner = 0U; corner < cube_values.size(); ++corner) {
                    cube_values[corner] = values[cube_indices[corner]];
                }

                for (const auto& tetrahedron : kTetrahedra) {
                    std::array<core::Vec3d, 4> tetra_positions{};
                    std::array<double, 4> tetra_values{};
                    for (std::size_t vertex = 0U; vertex < tetrahedron.size(); ++vertex) {
                        tetra_positions[vertex] = cube_positions[tetrahedron[vertex]];
                        tetra_values[vertex] = cube_values[tetrahedron[vertex]];
                    }
                    std::array<Crossing, 6> crossings{};
                    std::size_t crossing_count = 0U;
                    for (const auto& edge : kTetrahedronEdges) {
                        const bool first_above = tetra_values[edge[0]] > level;
                        const bool second_above = tetra_values[edge[1]] > level;
                        if (first_above == second_above) continue;
                        const double denominator = tetra_values[edge[1]] - tetra_values[edge[0]];
                        if (!std::isfinite(denominator) || denominator == 0.0) {
                            return core::Result<FieldIsosurfacePreview>::failure(validation(
                                "field isosurface preview encountered an invalid edge interval"));
                        }
                        const double fraction = std::clamp(
                            (level - tetra_values[edge[0]]) / denominator, 0.0, 1.0);
                        if (!std::isfinite(fraction)) {
                            return core::Result<FieldIsosurfacePreview>::failure(validation(
                                "field isosurface preview interpolation is not finite"));
                        }
                        crossings[crossing_count++] = Crossing{
                            edge[0], edge[1],
                            tetra_positions[edge[0]] +
                                (tetra_positions[edge[1]] - tetra_positions[edge[0]]) * fraction};
                    }
                    if (crossing_count == 3U) {
                        if (auto result = append_triangle(
                                crossings[0].point, crossings[1].point, crossings[2].point);
                            !result) {
                            return core::Result<FieldIsosurfacePreview>::failure(result.error());
                        }
                    } else if (crossing_count == 4U) {
                        std::array<std::size_t, 4> order{{0U, 0U, 0U, 0U}};
                        std::array<bool, 4> used{{false, false, false, false}};
                        order[0] = 0U;
                        used[0] = true;
                        std::size_t current_vertex = crossings[0].second;
                        for (std::size_t output = 1U; output < order.size(); ++output) {
                            std::size_t next = crossings.size();
                            for (std::size_t candidate = 0U; candidate < crossing_count; ++candidate) {
                                if (used[candidate]) continue;
                                if (crossings[candidate].first == current_vertex ||
                                    crossings[candidate].second == current_vertex) {
                                    next = candidate;
                                    break;
                                }
                            }
                            if (next == crossings.size()) {
                                return core::Result<FieldIsosurfacePreview>::failure(validation(
                                    "field isosurface preview crossing topology is ambiguous"));
                            }
                            order[output] = next;
                            used[next] = true;
                            current_vertex = crossings[next].first == current_vertex
                                ? crossings[next].second : crossings[next].first;
                        }
                        if (current_vertex != crossings[0].first) {
                            return core::Result<FieldIsosurfacePreview>::failure(validation(
                                "field isosurface preview crossing topology is not closed"));
                        }
                        if (auto result = append_triangle(
                                crossings[order[0]].point, crossings[order[1]].point,
                                crossings[order[2]].point);
                            !result) {
                            return core::Result<FieldIsosurfacePreview>::failure(result.error());
                        }
                        if (auto result = append_triangle(
                                crossings[order[0]].point, crossings[order[2]].point,
                                crossings[order[3]].point);
                            !result) {
                            return core::Result<FieldIsosurfacePreview>::failure(result.error());
                        }
                    } else if (crossing_count > 4U) {
                        return core::Result<FieldIsosurfacePreview>::failure(validation(
                            "field isosurface preview crossing count is invalid"));
                    }
                }
            }
        }
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldIsosurfacePreview>::failure(result.error());
    }
    return core::Result<FieldIsosurfacePreview>::success(std::move(preview));
}

core::Result<void> FieldVolumePreview::validate() const {
    if (auto result = validate_id(visualization.value, "field volume preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field volume preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field volume preview mode", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field volume preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "volume" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field volume preview supports only volume gray/grayscale/viridis data"));
    }
    const auto ray_samples = preview_volume_ray_sample_count(
        volume_width, volume_height, volume_depth, output_width, output_height,
        "field volume preview");
    if (!ray_samples) return core::Result<void>::failure(ray_samples.error());
    const auto output_samples = preview_grid_sample_count(
        output_width, output_height, "field volume preview");
    if (!output_samples) return core::Result<void>::failure(output_samples.error());
    if (pixels.size() != output_samples.value()) {
        return core::Result<void>::failure(validation(
            "field volume preview pixel count does not match its output grid"));
    }
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound ||
        !std::isfinite(opacity_scale) || opacity_scale < 0.0 || opacity_scale > 1.0) {
        return core::Result<void>::failure(validation(
            "field volume preview bounds or opacity scale are invalid"));
    }
    if (clipped_low > ray_samples.value() ||
        clipped_high > ray_samples.value() - clipped_low) {
        return core::Result<void>::failure(validation(
            "field volume preview clipping counts exceed its ray samples"));
    }
    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field volume preview range is not finite"));
    }
    std::uint64_t expected_clipped_low = 0U;
    std::uint64_t expected_clipped_high = 0U;
    const auto close = [](double left, double right) {
        return std::abs(left - right) <=
            1.0e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    for (const auto& pixel : pixels) {
        if (pixel.source_values.size() != volume_depth ||
            pixel.sample_count != volume_depth) {
            return core::Result<void>::failure(validation(
                "field volume preview pixel source samples or count are invalid"));
        }
        double expected_source_lower = std::numeric_limits<double>::infinity();
        double expected_source_upper = -std::numeric_limits<double>::infinity();
        std::array<double, 4> expected_rgba{0.0, 0.0, 0.0, 0.0};
        for (const double value : pixel.source_values) {
            if (!std::isfinite(value)) {
                return core::Result<void>::failure(validation(
                    "field volume preview pixel contains a non-finite source sample"));
            }
            expected_source_lower = std::min(expected_source_lower, value);
            expected_source_upper = std::max(expected_source_upper, value);
            double normalized = 0.5;
            if (range == 0.0) {
                if (value != lower_bound) {
                    return core::Result<void>::failure(validation(
                        "field volume preview zero-width range does not contain every sample"));
                }
            } else if (value < lower_bound) {
                normalized = 0.0;
                ++expected_clipped_low;
            } else if (value > upper_bound) {
                normalized = 1.0;
                ++expected_clipped_high;
            } else {
                normalized = (value - lower_bound) / range;
            }
            const auto color = preview_color(palette, normalized);
            if (!color) return core::Result<void>::failure(color.error());
            const double sample_opacity = normalized * opacity_scale;
            const double remaining = 1.0 - expected_rgba[3];
            expected_rgba[0] += remaining * sample_opacity * color.value()[0];
            expected_rgba[1] += remaining * sample_opacity * color.value()[1];
            expected_rgba[2] += remaining * sample_opacity * color.value()[2];
            expected_rgba[3] += remaining * sample_opacity;
        }
        if (!std::isfinite(pixel.source_lower) || !std::isfinite(pixel.source_upper) ||
            pixel.source_lower > pixel.source_upper ||
            !close(pixel.source_lower, expected_source_lower) ||
            !close(pixel.source_upper, expected_source_upper)) {
            return core::Result<void>::failure(validation(
                "field volume preview pixel source range does not match its samples"));
        }
        for (std::size_t channel = 0U; channel < pixel.rgba.size(); ++channel) {
            if (!std::isfinite(pixel.rgba[channel]) || pixel.rgba[channel] < 0.0 ||
                pixel.rgba[channel] > 1.0 || !close(pixel.rgba[channel], expected_rgba[channel])) {
                return core::Result<void>::failure(validation(
                    "field volume preview pixel color does not match its samples"));
            }
        }
        if (pixel.rgba[0] > pixel.rgba[3] + 1.0e-12 ||
            pixel.rgba[1] > pixel.rgba[3] + 1.0e-12 ||
            pixel.rgba[2] > pixel.rgba[3] + 1.0e-12) {
            return core::Result<void>::failure(validation(
                "field volume preview pixel is not premultiplied RGBA"));
        }
    }
    if (clipped_low != expected_clipped_low || clipped_high != expected_clipped_high) {
        return core::Result<void>::failure(validation(
            "field volume preview clipping counts do not match its samples"));
    }
    return core::Result<void>::success();
}

core::Result<FieldVolumePreview> FieldVisualization::preview_volume(
    std::span<const double> values,
    std::uint32_t volume_width,
    std::uint32_t volume_height,
    std::uint32_t volume_depth,
    std::uint32_t output_width,
    std::uint32_t output_height,
    double opacity_scale) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldVolumePreview>::failure(
            result.error().with_context("field volume preview"));
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<FieldVolumePreview>::failure(validation(
            "field volume preview requires a scalar gray/grayscale/viridis visualization"));
    }
    const auto volume_samples = preview_volume_sample_count(
        volume_width, volume_height, volume_depth, "field volume preview");
    if (!volume_samples) return core::Result<FieldVolumePreview>::failure(volume_samples.error());
    const auto output_samples = preview_grid_sample_count(
        output_width, output_height, "field volume preview");
    if (!output_samples) return core::Result<FieldVolumePreview>::failure(output_samples.error());
    const auto ray_samples = preview_volume_ray_sample_count(
        volume_width, volume_height, volume_depth, output_width, output_height,
        "field volume preview");
    if (!ray_samples) return core::Result<FieldVolumePreview>::failure(ray_samples.error());
    if (values.size() != volume_samples.value()) {
        return core::Result<FieldVolumePreview>::failure(validation(
            "field volume preview input count does not match its volume"));
    }
    if (!std::isfinite(opacity_scale) || opacity_scale < 0.0 || opacity_scale > 1.0) {
        return core::Result<FieldVolumePreview>::failure(validation(
            "field volume preview opacity scale is invalid"));
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return core::Result<FieldVolumePreview>::failure(validation(
                "field volume preview input contains a non-finite sample"));
        }
        observed_lower = std::min(observed_lower, value);
        observed_upper = std::max(observed_upper, value);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldVolumePreview>::failure(validation(
            "field volume preview bounds are invalid"));
    }

    FieldVolumePreview preview{
        id, field, "volume", palette, volume_width, volume_height, volume_depth,
        output_width, output_height, lower, upper, opacity_scale, 0U, 0U, {}};
    preview.pixels.reserve(output_samples.value());
    const auto sample_index = [volume_width, volume_height](
                                  std::size_t x, std::size_t y, std::size_t z) {
        return z * static_cast<std::size_t>(volume_width) *
                static_cast<std::size_t>(volume_height) +
            y * static_cast<std::size_t>(volume_width) + x;
    };
    const double range = upper - lower;
    for (std::size_t output_y = 0U; output_y < output_height; ++output_y) {
        const double normalized_y = static_cast<double>(output_y) /
            static_cast<double>(output_height - 1U);
        const auto volume_y = std::min(
            static_cast<std::size_t>(volume_height - 1U),
            static_cast<std::size_t>(normalized_y * static_cast<double>(volume_height - 1U) +
                                     0.5));
        for (std::size_t output_x = 0U; output_x < output_width; ++output_x) {
            const double normalized_x = static_cast<double>(output_x) /
                static_cast<double>(output_width - 1U);
            const auto volume_x = std::min(
                static_cast<std::size_t>(volume_width - 1U),
                static_cast<std::size_t>(normalized_x * static_cast<double>(volume_width - 1U) +
                                         0.5));
            FieldVolumePixel pixel;
            pixel.source_values.reserve(volume_depth);
            pixel.source_lower = std::numeric_limits<double>::infinity();
            pixel.source_upper = -std::numeric_limits<double>::infinity();
            pixel.sample_count = volume_depth;
            for (std::size_t volume_z = 0U; volume_z < volume_depth; ++volume_z) {
                const double value = values[sample_index(volume_x, volume_y, volume_z)];
                pixel.source_values.push_back(value);
                pixel.source_lower = std::min(pixel.source_lower, value);
                pixel.source_upper = std::max(pixel.source_upper, value);
                double normalized = 0.5;
                if (range == 0.0) {
                    if (value != lower) {
                        return core::Result<FieldVolumePreview>::failure(validation(
                            "field volume preview zero-width range does not contain every sample"));
                    }
                } else if (value < lower) {
                    normalized = 0.0;
                    ++preview.clipped_low;
                } else if (value > upper) {
                    normalized = 1.0;
                    ++preview.clipped_high;
                } else {
                    normalized = (value - lower) / range;
                }
                const auto color = preview_color(palette, normalized);
                if (!color) return core::Result<FieldVolumePreview>::failure(color.error());
                const double sample_opacity = normalized * opacity_scale;
                const double remaining = 1.0 - pixel.rgba[3];
                pixel.rgba[0] += remaining * sample_opacity * color.value()[0];
                pixel.rgba[1] += remaining * sample_opacity * color.value()[1];
                pixel.rgba[2] += remaining * sample_opacity * color.value()[2];
                pixel.rgba[3] += remaining * sample_opacity;
            }
            preview.pixels.push_back(std::move(pixel));
        }
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldVolumePreview>::failure(result.error());
    }
    return core::Result<FieldVolumePreview>::success(std::move(preview));
}

core::Result<void> FieldProbePreview::validate() const {
    if (auto result = validate_id(visualization.value, "field probe preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field probe preview field"); !result) {
        return result;
    }
    if (auto result = validate_id(probe.value, "field probe preview probe"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field probe preview mode", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field probe preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "probe" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field probe preview supports only probe gray/grayscale/viridis data"));
    }
    if (!position.finite()) {
        return core::Result<void>::failure(validation(
            "field probe preview position is not finite"));
    }
    if (!std::isfinite(source_value) || !std::isfinite(lower_bound) ||
        !std::isfinite(upper_bound) || lower_bound > upper_bound ||
        !std::isfinite(normalized_value) || normalized_value < 0.0 || normalized_value > 1.0) {
        return core::Result<void>::failure(validation(
            "field probe preview value or bounds are invalid"));
    }

    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field probe preview range is not finite"));
    }
    double expected_normalized = 0.5;
    bool expected_clipped_low = false;
    bool expected_clipped_high = false;
    if (range == 0.0) {
        if (source_value != lower_bound) {
            return core::Result<void>::failure(validation(
                "field probe preview zero-width range does not contain the sample"));
        }
    } else if (source_value < lower_bound) {
        expected_normalized = 0.0;
        expected_clipped_low = true;
    } else if (source_value > upper_bound) {
        expected_normalized = 1.0;
        expected_clipped_high = true;
    } else {
        expected_normalized = (source_value - lower_bound) / range;
    }
    if (std::abs(normalized_value - expected_normalized) > 1.0e-12 ||
        clipped_low != expected_clipped_low || clipped_high != expected_clipped_high) {
        return core::Result<void>::failure(validation(
            "field probe preview normalization or clipping does not match its sample"));
    }
    const auto expected_color = preview_color(palette, expected_normalized);
    if (!expected_color) return core::Result<void>::failure(expected_color.error());
    for (std::size_t channel = 0U; channel < rgba.size(); ++channel) {
        if (!std::isfinite(rgba[channel]) || rgba[channel] < 0.0 || rgba[channel] > 1.0 ||
            std::abs(rgba[channel] - expected_color.value()[channel]) > 1.0e-12) {
            return core::Result<void>::failure(validation(
                "field probe preview color does not match its palette"));
        }
    }
    return core::Result<void>::success();
}

core::Result<FieldProbePreview> FieldVisualization::preview_probe(
    ProbeId probe,
    core::Vec3d position,
    double value) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldProbePreview>::failure(
            result.error().with_context("field probe preview"));
    }
    if (mode != "scalar" || !supported_preview_palette(palette)) {
        return core::Result<FieldProbePreview>::failure(validation(
            "field probe preview requires a scalar gray/grayscale/viridis visualization"));
    }
    if (auto result = validate_id(probe.value, "field probe preview probe"); !result) {
        return core::Result<FieldProbePreview>::failure(result.error());
    }
    if (!position.finite() || !std::isfinite(value)) {
        return core::Result<FieldProbePreview>::failure(validation(
            "field probe preview input contains a non-finite position or value"));
    }

    const double lower = lower_bound.value_or(value);
    const double upper = upper_bound.value_or(value);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldProbePreview>::failure(validation(
            "field probe preview range is invalid"));
    }

    FieldProbePreview preview{
        id, field, probe, "probe", palette, position, value, lower, upper, 0.5,
        {0.0, 0.0, 0.0, 1.0}, false, false};
    const double range = upper - lower;
    if (range == 0.0) {
        if (value != lower) {
            return core::Result<FieldProbePreview>::failure(validation(
                "field probe preview zero-width range does not contain the sample"));
        }
    } else if (value < lower) {
        preview.normalized_value = 0.0;
        preview.clipped_low = true;
    } else if (value > upper) {
        preview.normalized_value = 1.0;
        preview.clipped_high = true;
    } else {
        preview.normalized_value = (value - lower) / range;
    }
    const auto color = preview_color(palette, preview.normalized_value);
    if (!color) return core::Result<FieldProbePreview>::failure(color.error());
    preview.rgba = color.value();
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldProbePreview>::failure(result.error());
    }
    return core::Result<FieldProbePreview>::success(std::move(preview));
}

core::Result<void> FieldStreamlinePreview::validate() const {
    if (auto result = validate_id(visualization.value, "field streamline preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field streamline preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field streamline preview mode", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field streamline preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "streamline" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field streamline preview supports only streamline gray/grayscale/viridis data"));
    }
    if (!preview_grid_sample_count(width, height, "field streamline preview")) {
        return core::Result<void>::failure(validation(
            "field streamline preview grid dimensions are invalid"));
    }
    constexpr std::uint32_t kMaxStreamlineSteps = 4096U;
    constexpr std::size_t kMaxStreamlinePoints = 4096U;
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound ||
        !std::isfinite(step_size) || step_size <= 0.0 || step_size > 1.0 || max_steps == 0U ||
        max_steps > kMaxStreamlineSteps) {
        return core::Result<void>::failure(validation(
            "field streamline preview bounds, step, or step count are invalid"));
    }
    if (lines.empty() || lines.size() > kMaxStreamlinePoints) {
        return core::Result<void>::failure(validation(
            "field streamline preview lines are empty or exceed the safety limit"));
    }
    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field streamline preview range is not finite"));
    }
    const auto close = [](double left, double right) {
        return std::abs(left - right) <=
            1.0e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    std::uint64_t expected_clipped_low = 0U;
    std::uint64_t expected_clipped_high = 0U;
    std::size_t total_points = 0U;
    for (const auto& line : lines) {
        if (line.points.empty() || line.points.size() > static_cast<std::size_t>(max_steps) + 1U ||
            total_points > kMaxCollectionEntries - line.points.size()) {
            return core::Result<void>::failure(validation(
                "field streamline preview line point count exceeds the safety limit"));
        }
        total_points += line.points.size();
        for (const auto& point : line.points) {
            if (!point.position.finite() || !point.vector.finite() ||
                point.position.x < 0.0 || point.position.x > 1.0 ||
                point.position.y < 0.0 || point.position.y > 1.0 ||
                point.position.z != 0.0 || !std::isfinite(point.magnitude) ||
                point.magnitude < 0.0 || !std::isfinite(point.normalized_magnitude) ||
                point.normalized_magnitude < 0.0 || point.normalized_magnitude > 1.0) {
                return core::Result<void>::failure(validation(
                    "field streamline preview contains a non-finite or out-of-domain point"));
            }
            const double expected_magnitude = point.vector.length();
            if (!std::isfinite(expected_magnitude) || !close(point.magnitude, expected_magnitude)) {
                return core::Result<void>::failure(validation(
                    "field streamline preview magnitude does not match its vector"));
            }
            double expected_normalized = 0.5;
            if (range == 0.0) {
                if (!close(point.magnitude, lower_bound)) {
                    return core::Result<void>::failure(validation(
                        "field streamline preview zero-width range does not contain every point"));
                }
            } else if (point.magnitude < lower_bound) {
                expected_normalized = 0.0;
                ++expected_clipped_low;
            } else if (point.magnitude > upper_bound) {
                expected_normalized = 1.0;
                ++expected_clipped_high;
            } else {
                expected_normalized = (point.magnitude - lower_bound) / range;
            }
            if (!close(point.normalized_magnitude, expected_normalized)) {
                return core::Result<void>::failure(validation(
                    "field streamline preview normalization does not match its magnitude"));
            }
            const auto expected_color = preview_color(palette, expected_normalized);
            if (!expected_color) return core::Result<void>::failure(expected_color.error());
            for (std::size_t channel = 0U; channel < point.rgba.size(); ++channel) {
                if (!std::isfinite(point.rgba[channel]) || point.rgba[channel] < 0.0 ||
                    point.rgba[channel] > 1.0 ||
                    !close(point.rgba[channel], expected_color.value()[channel])) {
                    return core::Result<void>::failure(validation(
                        "field streamline preview point color does not match its palette"));
                }
            }
        }
    }
    if (clipped_low != expected_clipped_low || clipped_high != expected_clipped_high) {
        return core::Result<void>::failure(validation(
            "field streamline preview clipping counts do not match its points"));
    }
    return core::Result<void>::success();
}

core::Result<FieldStreamlinePreview> FieldVisualization::preview_streamlines(
    std::span<const FieldVectorSample> samples,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const core::Vec3d> seeds,
    double step_size,
    std::uint32_t max_steps) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldStreamlinePreview>::failure(
            result.error().with_context("field streamline preview"));
    }
    if (mode != "vector" || !supported_preview_palette(palette)) {
        return core::Result<FieldStreamlinePreview>::failure(validation(
            "field streamline preview requires a vector gray/grayscale/viridis visualization"));
    }
    const auto sample_count = preview_grid_sample_count(width, height, "field streamline preview");
    if (!sample_count) return core::Result<FieldStreamlinePreview>::failure(sample_count.error());
    if (samples.size() != sample_count.value()) {
        return core::Result<FieldStreamlinePreview>::failure(validation(
            "field streamline preview input count does not match its grid"));
    }
    constexpr std::uint32_t kMaxStreamlineSteps = 4096U;
    constexpr std::size_t kMaxStreamlineSeeds = 4096U;
    if (seeds.empty() || seeds.size() > kMaxStreamlineSeeds ||
        !std::isfinite(step_size) || step_size <= 0.0 || step_size > 1.0 || max_steps == 0U ||
        max_steps > kMaxStreamlineSteps ||
        seeds.size() > kMaxCollectionEntries / (static_cast<std::size_t>(max_steps) + 1U)) {
        return core::Result<FieldStreamlinePreview>::failure(validation(
            "field streamline preview seeds, step, or output bound is invalid"));
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    const auto close = [](double left, double right) {
        return std::abs(left - right) <=
            1.0e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        const std::size_t x = index % static_cast<std::size_t>(width);
        const std::size_t y = index / static_cast<std::size_t>(width);
        const core::Vec3d expected_position{
            static_cast<double>(x) / static_cast<double>(width - 1U),
            static_cast<double>(y) / static_cast<double>(height - 1U), 0.0};
        if (!sample.position.finite() || !sample.vector.finite() || sample.vector.z != 0.0 ||
            !close(sample.position.x, expected_position.x) ||
            !close(sample.position.y, expected_position.y) || sample.position.z != 0.0) {
            return core::Result<FieldStreamlinePreview>::failure(validation(
                "field streamline preview input is not a finite normalized regular grid"));
        }
        const double magnitude = sample.vector.length();
        if (!std::isfinite(magnitude)) {
            return core::Result<FieldStreamlinePreview>::failure(validation(
                "field streamline preview input vector magnitude is not finite"));
        }
        observed_lower = std::min(observed_lower, magnitude);
        observed_upper = std::max(observed_upper, magnitude);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldStreamlinePreview>::failure(validation(
            "field streamline preview range is invalid"));
    }

    FieldStreamlinePreview preview{
        id, field, "streamline", palette, width, height, lower, upper, step_size, max_steps,
        0U, 0U, {}};
    preview.lines.reserve(seeds.size());
    const double range = upper - lower;
    const auto sample_vector = [&](core::Vec3d position) -> core::Result<core::Vec3d> {
        if (!position.finite() || position.x < 0.0 || position.x > 1.0 ||
            position.y < 0.0 || position.y > 1.0 || position.z != 0.0) {
            return core::Result<core::Vec3d>::failure(validation(
                "field streamline preview integration position is outside the grid"));
        }
        const double grid_x = position.x * static_cast<double>(width - 1U);
        const double grid_y = position.y * static_cast<double>(height - 1U);
        const std::size_t x0 = std::min(
            static_cast<std::size_t>(std::floor(grid_x)), static_cast<std::size_t>(width - 2U));
        const std::size_t y0 = std::min(
            static_cast<std::size_t>(std::floor(grid_y)), static_cast<std::size_t>(height - 2U));
        const double tx = grid_x - static_cast<double>(x0);
        const double ty = grid_y - static_cast<double>(y0);
        const auto at = [&](std::size_t x, std::size_t y) {
            return samples[y * static_cast<std::size_t>(width) + x].vector;
        };
        const auto lower_row = at(x0, y0) * (1.0 - tx) + at(x0 + 1U, y0) * tx;
        const auto upper_row = at(x0, y0 + 1U) * (1.0 - tx) + at(x0 + 1U, y0 + 1U) * tx;
        const auto result = lower_row * (1.0 - ty) + upper_row * ty;
        if (!result.finite()) {
            return core::Result<core::Vec3d>::failure(validation(
                "field streamline preview interpolation is not finite"));
        }
        return core::Result<core::Vec3d>::success(result);
    };
    const auto make_point = [&](core::Vec3d position, core::Vec3d vector)
        -> core::Result<FieldStreamlinePoint> {
        const double magnitude = vector.length();
        if (!position.finite() || !vector.finite() || !std::isfinite(magnitude)) {
            return core::Result<FieldStreamlinePoint>::failure(validation(
                "field streamline preview point is not finite"));
        }
        double normalized = 0.5;
        if (range == 0.0) {
            if (!close(magnitude, lower)) {
                return core::Result<FieldStreamlinePoint>::failure(validation(
                    "field streamline preview zero-width range does not contain the point"));
            }
        } else if (magnitude < lower) {
            normalized = 0.0;
            ++preview.clipped_low;
        } else if (magnitude > upper) {
            normalized = 1.0;
            ++preview.clipped_high;
        } else {
            normalized = (magnitude - lower) / range;
        }
        const auto color = preview_color(palette, normalized);
        if (!color) return core::Result<FieldStreamlinePoint>::failure(color.error());
        return core::Result<FieldStreamlinePoint>::success(
            FieldStreamlinePoint{position, vector, magnitude, normalized, color.value()});
    };

    for (const auto& seed : seeds) {
        if (!seed.finite() || seed.x < 0.0 || seed.x > 1.0 || seed.y < 0.0 || seed.y > 1.0 ||
            seed.z != 0.0) {
            return core::Result<FieldStreamlinePreview>::failure(validation(
                "field streamline preview seed is outside the normalized grid"));
        }
        FieldStreamline line;
        line.points.reserve(static_cast<std::size_t>(max_steps) + 1U);
        auto current = seed;
        const auto initial_vector = sample_vector(current);
        if (!initial_vector) {
            return core::Result<FieldStreamlinePreview>::failure(initial_vector.error());
        }
        const auto initial_point = make_point(current, initial_vector.value());
        if (!initial_point) {
            return core::Result<FieldStreamlinePreview>::failure(initial_point.error());
        }
        line.points.push_back(initial_point.value());
        for (std::uint32_t step = 0U; step < max_steps; ++step) {
            const auto vector = sample_vector(current);
            if (!vector) return core::Result<FieldStreamlinePreview>::failure(vector.error());
            const double magnitude = vector.value().length();
            if (magnitude == 0.0) break;
            const auto direction = vector.value().normalized();
            if (!direction.finite()) {
                return core::Result<FieldStreamlinePreview>::failure(validation(
                    "field streamline preview direction is not finite"));
            }
            const auto next = current + direction * step_size;
            if (!next.finite()) {
                return core::Result<FieldStreamlinePreview>::failure(validation(
                    "field streamline preview integration overflowed"));
            }
            if (next.x < 0.0 || next.x > 1.0 || next.y < 0.0 || next.y > 1.0 || next.z != 0.0) {
                line.terminated_by_boundary = true;
                break;
            }
            const auto next_vector = sample_vector(next);
            if (!next_vector) {
                return core::Result<FieldStreamlinePreview>::failure(next_vector.error());
            }
            const auto next_point = make_point(next, next_vector.value());
            if (!next_point) {
                return core::Result<FieldStreamlinePreview>::failure(next_point.error());
            }
            line.points.push_back(next_point.value());
            current = next;
        }
        preview.lines.push_back(std::move(line));
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldStreamlinePreview>::failure(result.error());
    }
    return core::Result<FieldStreamlinePreview>::success(std::move(preview));
}

core::Result<void> FieldVectorGlyphPreview::validate() const {
    if (auto result = validate_id(visualization.value, "field vector preview id"); !result) {
        return result;
    }
    if (auto result = validate_id(field.value, "field vector preview field"); !result) {
        return result;
    }
    if (auto result = validate_text(mode, "field vector preview mode", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(palette, "field vector preview palette", 128U, true);
        !result) {
        return result;
    }
    if (mode != "vector" || !supported_preview_palette(palette)) {
        return core::Result<void>::failure(validation(
            "field vector preview supports only vector gray/grayscale/viridis data"));
    }
    constexpr double kMaxGlyphScale = 1.0e6;
    if (!std::isfinite(lower_bound) || !std::isfinite(upper_bound) || lower_bound > upper_bound ||
        !std::isfinite(glyph_scale) || glyph_scale <= 0.0 || glyph_scale > kMaxGlyphScale) {
        return core::Result<void>::failure(validation(
            "field vector preview bounds or glyph scale are invalid"));
    }
    if (glyphs.empty() || glyphs.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "field vector preview glyphs are empty or exceed the safety limit"));
    }
    const auto glyph_count = static_cast<std::uint64_t>(glyphs.size());
    if (clipped_low > glyph_count || clipped_high > glyph_count - clipped_low ||
        zero_vectors > glyph_count) {
        return core::Result<void>::failure(validation(
            "field vector preview counts exceed the glyph count"));
    }

    const auto close = [](double left, double right) {
        return std::abs(left - right) <=
            1.0e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    const double range = upper_bound - lower_bound;
    if (!std::isfinite(range)) {
        return core::Result<void>::failure(validation(
            "field vector preview range is not finite"));
    }
    std::uint64_t expected_clipped_low = 0U;
    std::uint64_t expected_clipped_high = 0U;
    std::uint64_t expected_zero_vectors = 0U;
    for (const auto& glyph : glyphs) {
        if (!glyph.origin.finite() || !glyph.source_vector.finite() || !glyph.direction.finite() ||
            !std::isfinite(glyph.magnitude) || !std::isfinite(glyph.normalized_magnitude) ||
            !std::isfinite(glyph.length) || glyph.magnitude < 0.0 ||
            glyph.normalized_magnitude < 0.0 || glyph.normalized_magnitude > 1.0 ||
            glyph.length < 0.0) {
            return core::Result<void>::failure(validation(
                "field vector preview contains a non-finite or invalid glyph"));
        }
        const double expected_magnitude = glyph.source_vector.length();
        if (!std::isfinite(expected_magnitude) || !close(glyph.magnitude, expected_magnitude)) {
            return core::Result<void>::failure(validation(
                "field vector preview magnitude does not match its source vector"));
        }

        double expected_normalized = 0.5;
        if (range == 0.0) {
            if (!close(glyph.magnitude, lower_bound)) {
                return core::Result<void>::failure(validation(
                    "field vector preview zero-width range does not contain every magnitude"));
            }
        } else if (glyph.magnitude < lower_bound) {
            expected_normalized = 0.0;
            ++expected_clipped_low;
        } else if (glyph.magnitude > upper_bound) {
            expected_normalized = 1.0;
            ++expected_clipped_high;
        } else {
            expected_normalized = (glyph.magnitude - lower_bound) / range;
        }
        if (!close(glyph.normalized_magnitude, expected_normalized)) {
            return core::Result<void>::failure(validation(
                "field vector preview normalized magnitude does not match its source vector"));
        }

        if (expected_magnitude == 0.0) {
            ++expected_zero_vectors;
            if (glyph.direction.x != 0.0 || glyph.direction.y != 0.0 || glyph.direction.z != 0.0 ||
                glyph.length != 0.0) {
                return core::Result<void>::failure(validation(
                    "field vector preview zero vector has a non-zero glyph direction or length"));
            }
        } else {
            const auto expected_direction = glyph.source_vector.normalized();
            if (!expected_direction.finite() || !close(glyph.direction.x, expected_direction.x) ||
                !close(glyph.direction.y, expected_direction.y) ||
                !close(glyph.direction.z, expected_direction.z)) {
                return core::Result<void>::failure(validation(
                    "field vector preview direction does not match its source vector"));
            }
        }
        const double expected_length = expected_magnitude * glyph_scale;
        if (!std::isfinite(expected_length) || !close(glyph.length, expected_length)) {
            return core::Result<void>::failure(validation(
                "field vector preview glyph length does not match its scale"));
        }
        const auto expected_color = preview_color(palette, expected_normalized);
        if (!expected_color) return core::Result<void>::failure(expected_color.error());
        for (const double channel : glyph.rgba) {
            if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0) {
                return core::Result<void>::failure(validation(
                    "field vector preview glyph contains an invalid color"));
            }
        }
        for (std::size_t channel = 0U; channel < glyph.rgba.size(); ++channel) {
            if (!close(glyph.rgba[channel], expected_color.value()[channel])) {
                return core::Result<void>::failure(validation(
                    "field vector preview glyph color does not match its palette"));
            }
        }
    }
    if (clipped_low != expected_clipped_low || clipped_high != expected_clipped_high ||
        zero_vectors != expected_zero_vectors) {
        return core::Result<void>::failure(validation(
            "field vector preview counts do not match its glyphs"));
    }
    return core::Result<void>::success();
}

core::Result<FieldVectorGlyphPreview> FieldVisualization::preview_vector_glyphs(
    std::span<const FieldVectorSample> samples,
    double glyph_scale) const {
    if (auto result = validate(); !result) {
        return core::Result<FieldVectorGlyphPreview>::failure(
            result.error().with_context("field vector visualization preview"));
    }
    if (mode != "vector" || !supported_preview_palette(palette)) {
        return core::Result<FieldVectorGlyphPreview>::failure(validation(
            "field vector preview supports only vector gray/grayscale/viridis data"));
    }
    constexpr double kMaxGlyphScale = 1.0e6;
    if (!std::isfinite(glyph_scale) || glyph_scale <= 0.0 || glyph_scale > kMaxGlyphScale) {
        return core::Result<FieldVectorGlyphPreview>::failure(validation(
            "field vector preview glyph scale is invalid"));
    }
    if (samples.empty() || samples.size() > kMaxCollectionEntries) {
        return core::Result<FieldVectorGlyphPreview>::failure(validation(
            "field vector preview input is empty or exceeds the safety limit"));
    }

    double observed_lower = std::numeric_limits<double>::infinity();
    double observed_upper = -std::numeric_limits<double>::infinity();
    for (const auto& sample : samples) {
        if (!sample.position.finite() || !sample.vector.finite()) {
            return core::Result<FieldVectorGlyphPreview>::failure(validation(
                "field vector preview input contains a non-finite position or vector"));
        }
        const double magnitude = sample.vector.length();
        if (!std::isfinite(magnitude)) {
            return core::Result<FieldVectorGlyphPreview>::failure(validation(
                "field vector preview input vector magnitude is not finite"));
        }
        observed_lower = std::min(observed_lower, magnitude);
        observed_upper = std::max(observed_upper, magnitude);
    }
    const double lower = lower_bound.value_or(observed_lower);
    const double upper = upper_bound.value_or(observed_upper);
    if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
        !std::isfinite(upper - lower)) {
        return core::Result<FieldVectorGlyphPreview>::failure(validation(
            "field vector preview range is invalid"));
    }

    FieldVectorGlyphPreview preview{
        id, field, mode, palette, lower, upper, glyph_scale, 0U, 0U, 0U, {}};
    preview.glyphs.reserve(samples.size());
    const double range = upper - lower;
    for (const auto& sample : samples) {
        const double magnitude = sample.vector.length();
        double normalized = 0.5;
        if (range == 0.0) {
            if (magnitude != lower) {
                return core::Result<FieldVectorGlyphPreview>::failure(validation(
                    "field vector preview zero-width range does not contain every magnitude"));
            }
        } else if (magnitude < lower) {
            normalized = 0.0;
            ++preview.clipped_low;
        } else if (magnitude > upper) {
            normalized = 1.0;
            ++preview.clipped_high;
        } else {
            normalized = (magnitude - lower) / range;
        }
        FieldVectorGlyph glyph;
        glyph.origin = sample.position;
        glyph.source_vector = sample.vector;
        glyph.magnitude = magnitude;
        glyph.normalized_magnitude = normalized;
        if (magnitude == 0.0) {
            glyph.direction = {0.0, 0.0, 0.0};
            ++preview.zero_vectors;
        } else {
            glyph.direction = sample.vector.normalized();
            if (!glyph.direction.finite()) {
                return core::Result<FieldVectorGlyphPreview>::failure(validation(
                    "field vector preview normalized direction is not finite"));
            }
        }
        glyph.length = magnitude * glyph_scale;
        if (!std::isfinite(glyph.length)) {
            return core::Result<FieldVectorGlyphPreview>::failure(validation(
                "field vector preview glyph length is not finite"));
        }
        const auto color = preview_color(palette, normalized);
        if (!color) return core::Result<FieldVectorGlyphPreview>::failure(color.error());
        glyph.rgba = color.value();
        preview.glyphs.push_back(std::move(glyph));
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<FieldVectorGlyphPreview>::failure(result.error());
    }
    return core::Result<FieldVectorGlyphPreview>::success(std::move(preview));
}

core::Result<void> FieldVisualization::validate() const {
    if (auto result = validate_id(id.value, "field visualization id"); !result) return result;
    if (auto result = validate_id(field.value, "field visualization field"); !result) return result;
    if (auto result = validate_text(mode, "field visualization mode", 128U, true); !result) return result;
    if (auto result = validate_text(palette, "field visualization palette", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(stage, "field visualization stage", 256U); !result) return result;
    if (lower_bound.has_value() && !std::isfinite(*lower_bound)) {
        return core::Result<void>::failure(validation(
            "field visualization lower bound is not finite"));
    }
    if (upper_bound.has_value() && !std::isfinite(*upper_bound)) {
        return core::Result<void>::failure(validation(
            "field visualization upper bound is not finite"));
    }
    if (lower_bound.has_value() && upper_bound.has_value() && *lower_bound > *upper_bound) {
        return core::Result<void>::failure(validation(
            "field visualization bounds are inverted"));
    }
    return provenance.validate();
}

core::Result<void> FieldVisualizationModel::insert_visualization(FieldVisualization value) {
    return insert_unique(visualizations_, std::move(value), "field visualization");
}

core::Result<void> FieldVisualizationModel::validate() const {
    if (auto result = validate_text(schema, "field visualization schema", 128U, true); !result) {
        return result;
    }
    if (visualizations_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "field visualization collection exceeds the safety limit"));
    }
    for (const auto& [id, visualization] : visualizations_) {
        static_cast<void>(id);
        if (auto result = visualization.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("field visualization"));
        }
    }
    return core::Result<void>::success();
}

std::string FieldVisualizationModel::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTOGRAPHER_FIELD_VISUALIZATIONS " << kSchemaVersion << '\n';
    output << std::quoted(schema) << '\n';
    output << "VISUALIZATIONS " << visualizations_.size() << '\n';
    for (const auto& [id, visualization] : visualizations_) {
        output << "VISUALIZATION " << id.value << ' ' << visualization.field.value << ' '
               << optional_id(visualization.domain) << ' ' << std::quoted(visualization.mode) << ' '
               << std::quoted(visualization.palette) << ' '
               << (visualization.lower_bound.has_value() ? 1U : 0U);
        if (visualization.lower_bound.has_value()) output << ' ' << *visualization.lower_bound;
        output << ' ' << (visualization.upper_bound.has_value() ? 1U : 0U);
        if (visualization.upper_bound.has_value()) output << ' ' << *visualization.upper_bound;
        output << ' ' << std::quoted(visualization.stage) << ' '
               << (visualization.show_legend ? 1U : 0U) << ' ';
        write_provenance(output, visualization.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<FieldVisualizationModel> FieldVisualizationModel::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<FieldVisualizationModel>::failure(
            parse_error("field visualization record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_FIELD_VISUALIZATIONS"); !result) {
        return core::Result<FieldVisualizationModel>::failure(result.error());
    }
    const auto version = read_uint(input, "field visualization schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<FieldVisualizationModel>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported field visualization schema version"));
    }
    const auto schema = read_string(input, "field visualization schema", 128U, true);
    if (!schema) return core::Result<FieldVisualizationModel>::failure(schema.error());
    FieldVisualizationModel model;
    model.schema = schema.value();
    if (auto result = require_record(input, "VISUALIZATIONS"); !result) {
        return core::Result<FieldVisualizationModel>::failure(result.error());
    }
    const auto count = read_uint(input, "field visualization count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<FieldVisualizationModel>::failure(parse_error(
            "invalid field visualization count"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "VISUALIZATION"); !result) {
            return core::Result<FieldVisualizationModel>::failure(result.error());
        }
        const auto id = read_uint(input, "field visualization id");
        const auto field = read_uint(input, "field visualization field");
        const auto domain = read_optional_id<RegionId>(input, "field visualization domain");
        const auto mode = read_string(input, "field visualization mode", 128U, true);
        const auto palette = read_string(input, "field visualization palette", 128U, true);
        const auto lower_flag = read_uint(input, "field visualization lower flag");
        if (!id || !field || !domain || !mode || !palette || !lower_flag || lower_flag.value() > 1U) {
            return core::Result<FieldVisualizationModel>::failure(parse_error(
                "invalid field visualization record"));
        }
        std::optional<double> lower;
        if (lower_flag.value() != 0U) {
            const auto value = read_double(input, "field visualization lower bound");
            if (!value) return core::Result<FieldVisualizationModel>::failure(value.error());
            lower = value.value();
        }
        const auto upper_flag = read_uint(input, "field visualization upper flag");
        if (!upper_flag || upper_flag.value() > 1U) {
            return core::Result<FieldVisualizationModel>::failure(parse_error(
                "invalid field visualization upper flag"));
        }
        std::optional<double> upper;
        if (upper_flag.value() != 0U) {
            const auto value = read_double(input, "field visualization upper bound");
            if (!value) return core::Result<FieldVisualizationModel>::failure(value.error());
            upper = value.value();
        }
        const auto stage = read_string(input, "field visualization stage", 256U);
        const auto legend = read_uint(input, "field visualization legend flag");
        const auto provenance = read_provenance(input);
        if (!stage || !legend || legend.value() > 1U || !provenance) {
            return core::Result<FieldVisualizationModel>::failure(parse_error(
                "invalid field visualization metadata"));
        }
        const auto inserted = model.insert_visualization(FieldVisualization{
            VisualizationId{id.value()}, FieldId{field.value()}, domain.value(), mode.value(),
            palette.value(), lower, upper, stage.value(), legend.value() != 0U, provenance.value()});
        if (!inserted) return core::Result<FieldVisualizationModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<FieldVisualizationModel>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<FieldVisualizationModel>::failure(
            parse_error("field visualization record contains trailing data"));
    }
    if (auto result = model.validate(); !result) {
        return core::Result<FieldVisualizationModel>::failure(result.error());
    }
    return core::Result<FieldVisualizationModel>::success(std::move(model));
}

core::Result<void> ScientificImportReceipt::validate() const {
    if (auto result = validate_id(id.value, "scientific import receipt id"); !result) return result;
    if (auto result = validate_text(format, "scientific import format", 128U, true); !result) return result;
    if (auto result = validate_text(source_reference, "scientific import source reference",
                                    kMaxReferenceBytes, true); !result) return result;
    if (auto result = validate_text(provider, "scientific import provider", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_digest(source_digest, "scientific import source digest"); !result) {
        return result;
    }
    if (records_admitted > records_considered) {
        return core::Result<void>::failure(validation(
            "scientific import admitted record count exceeds considered count"));
    }
    const auto validate_notes = [](const std::vector<std::string>& notes,
                                   std::string_view field) -> core::Result<void> {
        if (notes.size() > kMaxTags) {
            return core::Result<void>::failure(validation(
                std::string(field) + " exceeds the note safety limit"));
        }
        std::set<std::string> unique;
        for (const auto& note : notes) {
            if (auto result = validate_text(note, field, kMaxTextBytes, true); !result) return result;
            if (!unique.insert(note).second) {
                return core::Result<void>::failure(validation(
                    std::string(field) + " contains a duplicate note"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_notes(warnings, "scientific import warnings"); !result) return result;
    if (auto result = validate_notes(feature_loss, "scientific import feature loss"); !result) return result;
    if (preservation_report.size() > kMaxTags) {
        return core::Result<void>::failure(validation(
            "scientific import preservation report exceeds the report safety limit"));
    }
    const auto is_known_disposition = [](std::string_view disposition) {
        return disposition == "preserved" || disposition == "converted" ||
            disposition == "ignored" || disposition == "unsupported" ||
            disposition == "ambiguous" || disposition == "repaired";
    };
    std::set<std::string> report_keys;
    for (const auto& entry : preservation_report) {
        if (auto result = validate_text(entry.source_key, "scientific import preservation source key",
                                        kMaxReferenceBytes, true); !result) {
            return result;
        }
        if (auto result = validate_text(entry.disposition, "scientific import preservation disposition",
                                        32U, true); !result) {
            return result;
        }
        if (!is_known_disposition(entry.disposition)) {
            return core::Result<void>::failure(validation(
                "scientific import preservation disposition is not recognized"));
        }
        if (auto result = validate_text(entry.target, "scientific import preservation target",
                                        kMaxReferenceBytes); !result) {
            return result;
        }
        if (auto result = validate_text(entry.note, "scientific import preservation note",
                                        kMaxTextBytes); !result) {
            return result;
        }
        if (!report_keys.insert(entry.source_key).second) {
            return core::Result<void>::failure(validation(
                "scientific import preservation report contains a duplicate source key"));
        }
    }
    return provenance.validate();
}

core::Result<void> ScientificImportLedger::insert_receipt(ScientificImportReceipt value) {
    return insert_unique(receipts_, std::move(value), "scientific import receipt");
}

core::Result<void> ScientificImportLedger::validate() const {
    if (receipts_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "scientific import receipt collection exceeds the safety limit"));
    }
    for (const auto& [id, receipt] : receipts_) {
        static_cast<void>(id);
        if (auto result = receipt.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific import receipt"));
        }
    }
    return core::Result<void>::success();
}

std::string ScientificImportLedger::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_SCIENTIFIC_IMPORTS " << kSchemaVersion << '\n';
    output << "RECEIPTS " << receipts_.size() << '\n';
    for (const auto& [id, receipt] : receipts_) {
        output << "IMPORT " << id.value << ' ' << std::quoted(receipt.format) << ' '
               << std::quoted(receipt.source_reference) << ' ' << std::quoted(receipt.provider);
        write_optional_digest(output, receipt.source_digest);
        output << ' ' << receipt.records_considered << ' ' << receipt.records_admitted << ' '
               << receipt.warnings.size();
        for (const auto& warning : receipt.warnings) output << ' ' << std::quoted(warning);
        output << ' ' << receipt.feature_loss.size();
        for (const auto& feature : receipt.feature_loss) output << ' ' << std::quoted(feature);
        output << ' ';
        write_provenance(output, receipt.provenance);
        output << ' ' << receipt.preservation_report.size();
        for (const auto& entry : receipt.preservation_report) {
            output << ' ' << std::quoted(entry.source_key) << ' '
                   << std::quoted(entry.disposition) << ' ' << std::quoted(entry.target) << ' '
                   << std::quoted(entry.note);
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ScientificImportLedger> ScientificImportLedger::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<ScientificImportLedger>::failure(
            parse_error("scientific import ledger is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_SCIENTIFIC_IMPORTS"); !result) {
        return core::Result<ScientificImportLedger>::failure(result.error());
    }
    const auto version = read_uint(input, "scientific import ledger schema version");
    if (!version || (version.value() != 1U && version.value() != kSchemaVersion)) {
        return core::Result<ScientificImportLedger>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported scientific import ledger schema version"));
    }
    ScientificImportLedger ledger;
    if (auto result = require_record(input, "RECEIPTS"); !result) {
        return core::Result<ScientificImportLedger>::failure(result.error());
    }
    const auto count = read_uint(input, "scientific import receipt count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<ScientificImportLedger>::failure(parse_error(
            "invalid scientific import receipt count"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "IMPORT"); !result) {
            return core::Result<ScientificImportLedger>::failure(result.error());
        }
        const auto id = read_uint(input, "scientific import receipt id");
        const auto format = read_string(input, "scientific import format", 128U, true);
        const auto source = read_string(input, "scientific import source reference", kMaxReferenceBytes, true);
        const auto provider = read_string(input, "scientific import provider", 256U, true);
        const auto source_digest = read_optional_digest(input, "scientific import source");
        const auto considered = read_uint(input, "scientific import records considered");
        const auto admitted = read_uint(input, "scientific import records admitted");
        const auto warning_count = read_uint(input, "scientific import warning count");
        if (!id || !format || !source || !provider || !source_digest || !considered || !admitted ||
            !warning_count || warning_count.value() > kMaxTags) {
            return core::Result<ScientificImportLedger>::failure(parse_error(
                "invalid scientific import receipt header"));
        }
        std::vector<std::string> warnings;
        warnings.reserve(static_cast<std::size_t>(warning_count.value()));
        for (std::uint64_t note = 0U; note < warning_count.value(); ++note) {
            const auto warning = read_string(input, "scientific import warning", kMaxTextBytes, true);
            if (!warning) return core::Result<ScientificImportLedger>::failure(warning.error());
            warnings.push_back(warning.value());
        }
        const auto feature_count = read_uint(input, "scientific import feature loss count");
        if (!feature_count || feature_count.value() > kMaxTags) {
            return core::Result<ScientificImportLedger>::failure(parse_error(
                "invalid scientific import feature loss count"));
        }
        std::vector<std::string> feature_loss;
        feature_loss.reserve(static_cast<std::size_t>(feature_count.value()));
        for (std::uint64_t feature = 0U; feature < feature_count.value(); ++feature) {
            const auto value = read_string(input, "scientific import feature loss", kMaxTextBytes, true);
            if (!value) return core::Result<ScientificImportLedger>::failure(value.error());
            feature_loss.push_back(value.value());
        }
        const auto provenance = read_provenance(input);
        if (!provenance) return core::Result<ScientificImportLedger>::failure(provenance.error());
        std::vector<ScientificImportDisposition> preservation_report;
        if (version.value() >= 2U) {
            const auto report_count = read_uint(input, "scientific import preservation report count");
            if (!report_count || report_count.value() > kMaxTags) {
                return core::Result<ScientificImportLedger>::failure(parse_error(
                    "invalid scientific import preservation report count"));
            }
            preservation_report.reserve(static_cast<std::size_t>(report_count.value()));
            for (std::uint64_t entry = 0U; entry < report_count.value(); ++entry) {
                const auto source_key = read_string(
                    input, "scientific import preservation source key", kMaxReferenceBytes, true);
                const auto disposition = read_string(
                    input, "scientific import preservation disposition", 32U, true);
                const auto target = read_string(
                    input, "scientific import preservation target", kMaxReferenceBytes);
                const auto note = read_string(
                    input, "scientific import preservation note", kMaxTextBytes);
                if (!source_key || !disposition || !target || !note) {
                    const auto& error = !source_key ? source_key.error() :
                        !disposition ? disposition.error() : !target ? target.error() : note.error();
                    return core::Result<ScientificImportLedger>::failure(error);
                }
                preservation_report.push_back(ScientificImportDisposition{
                    source_key.value(), disposition.value(), target.value(), note.value()});
            }
        }
        const auto inserted = ledger.insert_receipt(ScientificImportReceipt{
            ImportId{id.value()}, format.value(), source.value(), provider.value(), source_digest.value(),
            considered.value(), admitted.value(), std::move(warnings), std::move(feature_loss),
            provenance.value(), std::move(preservation_report)});
        if (!inserted) return core::Result<ScientificImportLedger>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<ScientificImportLedger>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ScientificImportLedger>::failure(
            parse_error("scientific import ledger contains trailing data"));
    }
    if (auto result = ledger.validate(); !result) {
        return core::Result<ScientificImportLedger>::failure(result.error());
    }
    return core::Result<ScientificImportLedger>::success(std::move(ledger));
}

namespace {

assets::Sha256Digest functional_genome_semantic_digest(const FunctionalGenome& genome) {
    const std::string encoded = genome.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()});
}

} // namespace

core::Result<FunctionalGenomeExchange> FunctionalGenomeExchange::from_import(
    FunctionalGenome genome,
    ScientificImportReceipt import_receipt) {
    FunctionalGenomeExchange exchange{
        std::move(genome), std::move(import_receipt), assets::Sha256Digest{}};
    exchange.semantic_digest = functional_genome_semantic_digest(exchange.genome);
    if (auto result = exchange.validate(); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    return core::Result<FunctionalGenomeExchange>::success(std::move(exchange));
}

core::Result<void> FunctionalGenomeExchange::validate() const {
    if (auto result = genome.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "functional genome exchange genome"));
    }
    if (auto result = import_receipt.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "functional genome exchange import receipt"));
    }
    if (!import_receipt.source_digest.has_value()) {
        return core::Result<void>::failure(validation(
            "functional genome exchange requires a source digest"));
    }
    if (import_receipt.preservation_report.empty()) {
        return core::Result<void>::failure(validation(
            "functional genome exchange requires a preservation report"));
    }
    if (semantic_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "functional genome exchange semantic digest must be non-zero"));
    }
    if (functional_genome_semantic_digest(genome) != semantic_digest) {
        return core::Result<void>::failure(validation(
            "functional genome exchange semantic digest does not match the canonical genome"));
    }
    return core::Result<void>::success();
}

std::string FunctionalGenomeExchange::serialize() const {
    if (auto result = validate(); !result) return {};
    ScientificImportLedger ledger;
    if (auto result = ledger.insert_receipt(import_receipt); !result) return {};

    std::ostringstream output;
    output << "CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE " << kSchemaVersion << '\n';
    output << "SEMANTIC_DIGEST " << semantic_digest.hex() << '\n';
    output << "IMPORT_LEDGER " << std::quoted(ledger.serialize()) << '\n';
    output << "FUNCTIONAL_GENOME " << std::quoted(genome.serialize()) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<FunctionalGenomeExchange> FunctionalGenomeExchange::deserialize(
    std::string_view text) {
    constexpr std::size_t kMaxExchangeBytes = 8U * 1024U * 1024U;
    if (text.size() > kMaxExchangeBytes) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "functional genome exchange is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE"); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    const auto version = read_uint(input, "functional genome exchange schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<FunctionalGenomeExchange>::failure(
            Diagnostic(ErrorCode::version_mismatch,
                       "unsupported functional genome exchange schema version"));
    }
    if (auto result = require_record(input, "SEMANTIC_DIGEST"); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    std::string encoded_digest;
    if (!(input >> encoded_digest)) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "functional genome exchange semantic digest is missing"));
    }
    const auto semantic_digest = assets::Sha256Digest::from_hex(encoded_digest);
    if (!semantic_digest || semantic_digest.value().is_zero()) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "functional genome exchange semantic digest is invalid"));
    }

    if (auto result = require_record(input, "IMPORT_LEDGER"); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    std::string encoded_ledger;
    if (!(input >> std::quoted(encoded_ledger)) || encoded_ledger.size() > 4U * 1024U * 1024U) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "invalid functional genome exchange import ledger"));
    }
    const auto ledger = ScientificImportLedger::deserialize(encoded_ledger);
    if (!ledger || ledger.value().receipts().size() != 1U) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "functional genome exchange must contain exactly one import receipt"));
    }

    if (auto result = require_record(input, "FUNCTIONAL_GENOME"); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    std::string encoded_genome;
    if (!(input >> std::quoted(encoded_genome)) || encoded_genome.size() > 4U * 1024U * 1024U) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "invalid functional genome exchange genome"));
    }
    const auto genome = FunctionalGenome::deserialize(encoded_genome);
    if (!genome) {
        return core::Result<FunctionalGenomeExchange>::failure(
            genome.error().with_context("functional genome exchange genome"));
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<FunctionalGenomeExchange>::failure(parse_error(
            "functional genome exchange contains trailing data"));
    }

    FunctionalGenomeExchange exchange{
        genome.value(), ledger.value().receipts().begin()->second, semantic_digest.value()};
    if (auto result = exchange.validate(); !result) {
        return core::Result<FunctionalGenomeExchange>::failure(result.error());
    }
    return core::Result<FunctionalGenomeExchange>::success(std::move(exchange));
}

core::Result<void> PhenotypeCapability::validate() const {
    if (auto result = validate_id(id.value, "phenotype capability id"); !result) return result;
    if (auto result = validate_text(name, "phenotype capability name", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_text(category, "phenotype capability category", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(state, "phenotype capability state", 128U, true); !result) {
        return result;
    }
    if (value.has_value() && !std::isfinite(*value)) {
        return core::Result<void>::failure(validation(
            "phenotype capability value must be finite"));
    }
    if (auto result = validate_text(units, "phenotype capability units", 128U); !result) {
        return result;
    }
    if (auto result = validate_id_list(contributing_structures,
                                      "phenotype capability contributing structures"); !result) {
        return result;
    }
    if (auto result = validate_id_list(contributing_genes,
                                      "phenotype capability contributing genes"); !result) {
        return result;
    }
    if (auto result = validate_id_list(contributing_instructions,
                                      "phenotype capability contributing instructions"); !result) {
        return result;
    }
    if (auto result = validate_id_list(contributing_traces,
                                      "phenotype capability contributing traces"); !result) {
        return result;
    }
    if (contributing_structures.empty() && contributing_genes.empty() &&
        contributing_instructions.empty() && contributing_traces.empty()) {
        return core::Result<void>::failure(validation(
            "phenotype capability must retain at least one contributing source"));
    }
    if (auto result = validate_text(derivation, "phenotype capability derivation",
                                    kMaxTextBytes, true); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "phenotype capability provenance must be explicit"));
    }
    return provenance.validate();
}

core::Result<void> PhenotypeCapabilityModel::insert_capability(PhenotypeCapability value) {
    return insert_unique(capabilities_, std::move(value), "phenotype capability");
}

core::Result<PhenotypeCapability> PhenotypeCapabilityModel::trace(CapabilityId id) const {
    if (auto result = validate(); !result) {
        return core::Result<PhenotypeCapability>::failure(
            result.error().with_context("phenotype capability trace"));
    }
    if (auto result = validate_id(id.value, "phenotype capability trace id"); !result) {
        return core::Result<PhenotypeCapability>::failure(result.error());
    }
    const auto iterator = capabilities_.find(id);
    if (iterator == capabilities_.end()) {
        return core::Result<PhenotypeCapability>::failure(validation(
            "phenotype capability trace references an unknown capability"));
    }
    return core::Result<PhenotypeCapability>::success(iterator->second);
}

namespace {

template <typename IdType, typename Predicate>
core::Result<std::vector<CapabilityId>> reverse_capability_trace(
    const PhenotypeCapabilityModel& model,
    IdType source_id,
    std::string_view source_label,
    Predicate predicate) {
    if (auto result = validate_id(source_id.value, source_label); !result) {
        return core::Result<std::vector<CapabilityId>>::failure(result.error());
    }
    if (auto result = model.validate(); !result) {
        return core::Result<std::vector<CapabilityId>>::failure(
            result.error().with_context("reverse phenotype capability trace"));
    }
    std::vector<CapabilityId> result;
    for (const auto& [capability_id, capability] : model.capabilities()) {
        if (predicate(capability, source_id)) result.push_back(capability_id);
    }
    return core::Result<std::vector<CapabilityId>>::success(std::move(result));
}

} // namespace

core::Result<std::vector<CapabilityId>> PhenotypeCapabilityModel::capabilities_for_structure(
    StructureId id) const {
    return reverse_capability_trace(
        *this, id, "phenotype capability structure trace id",
        [](const PhenotypeCapability& capability, StructureId source) {
            return std::find(capability.contributing_structures.begin(),
                             capability.contributing_structures.end(), source) !=
                capability.contributing_structures.end();
        });
}

core::Result<std::vector<CapabilityId>> PhenotypeCapabilityModel::capabilities_for_gene(
    GeneId id) const {
    return reverse_capability_trace(
        *this, id, "phenotype capability gene trace id",
        [](const PhenotypeCapability& capability, GeneId source) {
            return std::find(capability.contributing_genes.begin(),
                             capability.contributing_genes.end(), source) !=
                capability.contributing_genes.end();
        });
}

core::Result<std::vector<CapabilityId>> PhenotypeCapabilityModel::capabilities_for_instruction(
    InstructionId id) const {
    return reverse_capability_trace(
        *this, id, "phenotype capability instruction trace id",
        [](const PhenotypeCapability& capability, InstructionId source) {
            return std::find(capability.contributing_instructions.begin(),
                             capability.contributing_instructions.end(), source) !=
                capability.contributing_instructions.end();
        });
}

core::Result<std::vector<CapabilityId>> PhenotypeCapabilityModel::capabilities_for_trace(
    TraceId id) const {
    return reverse_capability_trace(
        *this, id, "phenotype capability development trace id",
        [](const PhenotypeCapability& capability, TraceId source) {
            return std::find(capability.contributing_traces.begin(),
                             capability.contributing_traces.end(), source) !=
                capability.contributing_traces.end();
        });
}

core::Result<void> PhenotypeCapabilityModel::validate() const {
    if (auto result = validate_text(schema, "phenotype capability schema", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_text(compilation_reference,
                                    "phenotype capability compilation reference",
                                    kMaxReferenceBytes, true); !result) {
        return result;
    }
    if (auto result = validate_text(status, "phenotype capability status", 128U, true);
        !result) {
        return result;
    }
    if (auto result = validate_digest(source_genome_digest,
                                      "phenotype capability source genome digest"); !result) {
        return result;
    }
    if (auto result = validate_digest(source_morphology_digest,
                                      "phenotype capability source morphology digest"); !result) {
        return result;
    }
    if (auto result = validate_digest(source_development_digest,
                                      "phenotype capability source development digest"); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "phenotype capability model provenance must be explicit"));
    }
    if (auto result = provenance.validate(); !result) return result;
    if (capabilities_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "phenotype capability collection exceeds the safety limit"));
    }
    for (const auto& [id, capability] : capabilities_) {
        static_cast<void>(id);
        if (auto result = capability.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("phenotype capability"));
        }
    }
    return core::Result<void>::success();
}

std::string PhenotypeCapabilityModel::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_PHENOTYPE_CAPABILITIES " << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema) << '\n';
    output << "COMPILATION_REFERENCE " << std::quoted(compilation_reference) << '\n';
    output << "STATUS " << std::quoted(status) << '\n';
    output << "SOURCE_GENOME_DIGEST";
    write_optional_digest(output, source_genome_digest);
    output << '\n';
    output << "SOURCE_MORPHOLOGY_DIGEST";
    write_optional_digest(output, source_morphology_digest);
    output << '\n';
    output << "SOURCE_DEVELOPMENT_DIGEST";
    write_optional_digest(output, source_development_digest);
    output << '\n';
    output << "PROVENANCE ";
    write_provenance(output, provenance);
    output << '\n';
    output << "CAPABILITIES " << capabilities_.size() << '\n';
    for (const auto& [id, capability] : capabilities_) {
        output << "CAPABILITY " << id.value << ' ' << std::quoted(capability.name) << ' '
               << std::quoted(capability.category) << ' ' << std::quoted(capability.state) << ' '
               << (capability.value.has_value() ? 1U : 0U);
        if (capability.value.has_value()) output << ' ' << *capability.value;
        output << ' ' << std::quoted(capability.units) << ' ';
        write_id_list(output, capability.contributing_structures);
        output << ' ';
        write_id_list(output, capability.contributing_genes);
        output << ' ';
        write_id_list(output, capability.contributing_instructions);
        output << ' ';
        write_id_list(output, capability.contributing_traces);
        output << ' ' << std::quoted(capability.derivation) << ' ';
        write_provenance(output, capability.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<PhenotypeCapabilityModel> PhenotypeCapabilityModel::deserialize(
    std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<PhenotypeCapabilityModel>::failure(
            parse_error("phenotype capability model is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_PHENOTYPE_CAPABILITIES"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    const auto version = read_uint(input, "phenotype capability schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<PhenotypeCapabilityModel>::failure(Diagnostic(
            ErrorCode::version_mismatch,
            "unsupported phenotype capability schema version"));
    }
    if (auto result = require_record(input, "SCHEMA"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    const auto schema = read_string(input, "phenotype capability schema", 128U, true);
    if (auto result = require_record(input, "COMPILATION_REFERENCE"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    const auto compilation_reference = read_string(
        input, "phenotype capability compilation reference", kMaxReferenceBytes, true);
    if (auto result = require_record(input, "STATUS"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    const auto status = read_string(input, "phenotype capability status", 128U, true);
    if (!schema || !compilation_reference || !status) {
        const auto& error = !schema ? schema.error() :
            !compilation_reference ? compilation_reference.error() : status.error();
        return core::Result<PhenotypeCapabilityModel>::failure(error);
    }
    const auto read_digest_record = [&input](std::string_view record,
                                             std::string_view field) {
        if (auto result = require_record(input, record); !result) {
            return core::Result<std::optional<assets::Sha256Digest>>::failure(result.error());
        }
        return read_optional_digest(input, field);
    };
    const auto genome_digest = read_digest_record(
        "SOURCE_GENOME_DIGEST", "phenotype capability source genome");
    const auto morphology_digest = read_digest_record(
        "SOURCE_MORPHOLOGY_DIGEST", "phenotype capability source morphology");
    const auto development_digest = read_digest_record(
        "SOURCE_DEVELOPMENT_DIGEST", "phenotype capability source development");
    if (!genome_digest || !morphology_digest || !development_digest) {
        const auto& error = !genome_digest ? genome_digest.error() :
            !morphology_digest ? morphology_digest.error() : development_digest.error();
        return core::Result<PhenotypeCapabilityModel>::failure(error);
    }
    if (auto result = require_record(input, "PROVENANCE"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    const auto provenance = read_provenance(input);
    if (!provenance) {
        return core::Result<PhenotypeCapabilityModel>::failure(provenance.error());
    }
    PhenotypeCapabilityModel model;
    model.schema = schema.value();
    model.compilation_reference = compilation_reference.value();
    model.status = status.value();
    model.source_genome_digest = genome_digest.value();
    model.source_morphology_digest = morphology_digest.value();
    model.source_development_digest = development_digest.value();
    model.provenance = provenance.value();
    if (auto result = require_record(input, "CAPABILITIES"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    const auto count = read_uint(input, "phenotype capability count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<PhenotypeCapabilityModel>::failure(parse_error(
            "invalid phenotype capability count"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "CAPABILITY"); !result) {
            return core::Result<PhenotypeCapabilityModel>::failure(result.error());
        }
        const auto id = read_uint(input, "phenotype capability id");
        const auto name = read_string(input, "phenotype capability name", 256U, true);
        const auto category = read_string(input, "phenotype capability category", 128U, true);
        const auto state = read_string(input, "phenotype capability state", 128U, true);
        const auto value_present = read_uint(input, "phenotype capability value presence");
        if (!id || !name || !category || !state || !value_present || value_present.value() > 1U) {
            return core::Result<PhenotypeCapabilityModel>::failure(parse_error(
                "invalid phenotype capability header"));
        }
        std::optional<double> value;
        if (value_present.value() != 0U) {
            const auto parsed = read_double(input, "phenotype capability value");
            if (!parsed) return core::Result<PhenotypeCapabilityModel>::failure(parsed.error());
            value = parsed.value();
        }
        const auto units = read_string(input, "phenotype capability units", 128U);
        const auto structures = read_id_list<StructureId>(
            input, "phenotype capability contributing structures");
        const auto genes = read_id_list<GeneId>(
            input, "phenotype capability contributing genes");
        const auto instructions = read_id_list<InstructionId>(
            input, "phenotype capability contributing instructions");
        const auto traces = read_id_list<TraceId>(
            input, "phenotype capability contributing traces");
        const auto derivation = read_string(input, "phenotype capability derivation",
                                             kMaxTextBytes, true);
        const auto capability_provenance = read_provenance(input);
        if (!units || !structures || !genes || !instructions || !traces || !derivation ||
            !capability_provenance) {
            const auto& error = !units ? units.error() : !structures ? structures.error() :
                !genes ? genes.error() : !instructions ? instructions.error() :
                !traces ? traces.error() : !derivation ? derivation.error() :
                capability_provenance.error();
            return core::Result<PhenotypeCapabilityModel>::failure(error);
        }
        const auto inserted = model.insert_capability(PhenotypeCapability{
            CapabilityId{id.value()}, name.value(), category.value(), state.value(), value,
            units.value(), structures.value(), genes.value(), instructions.value(), traces.value(),
            derivation.value(), capability_provenance.value()});
        if (!inserted) return core::Result<PhenotypeCapabilityModel>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PhenotypeCapabilityModel>::failure(
            parse_error("phenotype capability model contains trailing data"));
    }
    if (auto result = model.validate(); !result) {
        return core::Result<PhenotypeCapabilityModel>::failure(result.error());
    }
    return core::Result<PhenotypeCapabilityModel>::success(std::move(model));
}

core::Result<void> RegulatoryElement::validate() const {
    if (auto result = validate_id(id.value, "regulatory element id"); !result) return result;
    if (auto result = validate_id(gene.value, "regulatory element gene"); !result) return result;
    if (auto result = validate_text(name, "regulatory element name", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_text(kind, "regulatory element kind", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(source_reference, "regulatory element source reference",
                                    kMaxReferenceBytes, true); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "regulatory element provenance must be explicit"));
    }
    return provenance.validate();
}

core::Result<void> RegulatoryEdge::validate() const {
    if (auto result = validate_id(id.value, "regulatory edge id"); !result) return result;
    if (auto result = validate_id(source_gene.value, "regulatory edge source gene"); !result) {
        return result;
    }
    if (auto result = validate_id(target_gene.value, "regulatory edge target gene"); !result) {
        return result;
    }
    if (auto result = validate_id(element.value, "regulatory edge element"); !result) return result;
    if (mode != "activation" && mode != "repression") {
        return core::Result<void>::failure(validation(
            "regulatory edge mode must be activation or repression"));
    }
    if (auto result = validate_text(condition, "regulatory edge condition", kMaxTextBytes);
        !result) {
        return result;
    }
    if (stage.has_value() && !stage->value) {
        return core::Result<void>::failure(validation(
            "regulatory edge stage must be non-zero"));
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "regulatory edge provenance must be explicit"));
    }
    return provenance.validate();
}

core::Result<void> SpatialExpression::validate() const {
    if (auto result = validate_id(id.value, "spatial expression id"); !result) return result;
    if (auto result = validate_id(gene.value, "spatial expression gene"); !result) return result;
    const auto target_count = static_cast<unsigned>(structure.has_value()) +
        static_cast<unsigned>(region.has_value()) + static_cast<unsigned>(field.has_value());
    if (target_count != 1U) {
        return core::Result<void>::failure(validation(
            "spatial expression must target exactly one structure, region, or field"));
    }
    if (stage.has_value() && !stage->value) {
        return core::Result<void>::failure(validation(
            "spatial expression stage must be non-zero"));
    }
    if (mode != "structure_scalar" && mode != "region_scalar" &&
        mode != "continuous_field" && mode != "time_dependent_field" &&
        mode != "categorical_state") {
        return core::Result<void>::failure(validation(
            "spatial expression mode is unsupported"));
    }
    if (mode == "structure_scalar" && !structure.has_value()) {
        return core::Result<void>::failure(validation(
            "structure_scalar expression requires a structure target"));
    }
    if (mode == "region_scalar" && !region.has_value()) {
        return core::Result<void>::failure(validation(
            "region_scalar expression requires a region target"));
    }
    if ((mode == "continuous_field" || mode == "time_dependent_field") && !field.has_value()) {
        return core::Result<void>::failure(validation(
            "field expression requires a field target"));
    }
    if (mode == "categorical_state" && categorical_state.empty()) {
        return core::Result<void>::failure(validation(
            "categorical expression requires a categorical state"));
    }
    if (value.has_value() && !std::isfinite(*value)) {
        return core::Result<void>::failure(validation(
            "spatial expression value must be finite"));
    }
    if (auto result = validate_text(categorical_state, "spatial expression categorical state",
                                    128U); !result) {
        return result;
    }
    if (auto result = validate_text(units, "spatial expression units", 128U); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "spatial expression provenance must be explicit"));
    }
    return provenance.validate();
}

core::Result<void> GeneRegulatoryNetwork::insert_element(RegulatoryElement value) {
    return insert_unique(elements_, std::move(value), "regulatory element");
}

core::Result<void> GeneRegulatoryNetwork::insert_edge(RegulatoryEdge value) {
    return insert_unique(edges_, std::move(value), "regulatory edge");
}

core::Result<void> GeneRegulatoryNetwork::insert_expression(SpatialExpression value) {
    return insert_unique(expressions_, std::move(value), "spatial expression");
}

core::Result<void> GeneRegulatoryNetwork::validate() const {
    if (auto result = validate_text(schema, "regulatory network schema", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(reference, "regulatory network reference",
                                    kMaxReferenceBytes, true); !result) {
        return result;
    }
    if (auto result = validate_text(status, "regulatory network status", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_digest(source_digest, "regulatory network source digest"); !result) {
        return result;
    }
    if (provenance.empty()) {
        return core::Result<void>::failure(validation(
            "regulatory network provenance must be explicit"));
    }
    if (auto result = provenance.validate(); !result) return result;
    if (elements_.size() > kMaxCollectionEntries || edges_.size() > kMaxCollectionEntries ||
        expressions_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "regulatory network collection exceeds the safety limit"));
    }
    for (const auto& [id, element] : elements_) {
        static_cast<void>(id);
        if (auto result = element.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("regulatory element"));
        }
    }
    for (const auto& [id, edge] : edges_) {
        static_cast<void>(id);
        if (auto result = edge.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("regulatory edge"));
        }
        if (auto result = validate_reference(elements_, edge.element, "regulatory edge element");
            !result) {
            return result;
        }
    }
    for (const auto& [id, expression] : expressions_) {
        static_cast<void>(id);
        if (auto result = expression.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("spatial expression"));
        }
    }
    return core::Result<void>::success();
}

std::string GeneRegulatoryNetwork::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_GENE_REGULATORY_NETWORK " << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema) << '\n';
    output << "REFERENCE " << std::quoted(reference) << '\n';
    output << "STATUS " << std::quoted(status) << '\n';
    output << "SOURCE_DIGEST";
    write_optional_digest(output, source_digest);
    output << '\n';
    output << "PROVENANCE ";
    write_provenance(output, provenance);
    output << '\n';
    output << "ELEMENTS " << elements_.size() << '\n';
    for (const auto& [id, element] : elements_) {
        output << "ELEMENT " << id.value << ' ' << element.gene.value << ' '
               << std::quoted(element.name) << ' ' << std::quoted(element.kind) << ' '
               << std::quoted(element.source_reference) << ' ';
        write_provenance(output, element.provenance);
        output << '\n';
    }
    output << "EDGES " << edges_.size() << '\n';
    for (const auto& [id, edge] : edges_) {
        output << "EDGE " << id.value << ' ' << edge.source_gene.value << ' '
               << edge.target_gene.value << ' ' << edge.element.value << ' '
               << std::quoted(edge.mode) << ' ' << std::quoted(edge.condition) << ' '
               << optional_id(edge.stage) << ' ';
        write_provenance(output, edge.provenance);
        output << '\n';
    }
    output << "EXPRESSIONS " << expressions_.size() << '\n';
    for (const auto& [id, expression] : expressions_) {
        output << "EXPRESSION " << id.value << ' ' << expression.gene.value << ' '
               << optional_id(expression.structure) << ' ' << optional_id(expression.region) << ' '
               << optional_id(expression.field) << ' ' << optional_id(expression.stage) << ' '
               << std::quoted(expression.mode) << ' '
               << (expression.value.has_value() ? 1U : 0U);
        if (expression.value.has_value()) output << ' ' << *expression.value;
        output << ' ' << std::quoted(expression.categorical_state) << ' '
               << std::quoted(expression.units) << ' ';
        write_provenance(output, expression.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<GeneRegulatoryNetwork> GeneRegulatoryNetwork::deserialize(std::string_view text) {
    if (text.size() > 8U * 1024U * 1024U) {
        return core::Result<GeneRegulatoryNetwork>::failure(
            parse_error("gene regulatory network is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_GENE_REGULATORY_NETWORK"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto version = read_uint(input, "regulatory network schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<GeneRegulatoryNetwork>::failure(Diagnostic(
            ErrorCode::version_mismatch, "unsupported regulatory network schema version"));
    }
    if (auto result = require_record(input, "SCHEMA"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto schema = read_string(input, "regulatory network schema", 128U, true);
    if (auto result = require_record(input, "REFERENCE"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto reference = read_string(input, "regulatory network reference",
                                       kMaxReferenceBytes, true);
    if (auto result = require_record(input, "STATUS"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto status = read_string(input, "regulatory network status", 128U, true);
    if (!schema || !reference || !status) {
        const auto& error = !schema ? schema.error() : !reference ? reference.error() : status.error();
        return core::Result<GeneRegulatoryNetwork>::failure(error);
    }
    if (auto result = require_record(input, "SOURCE_DIGEST"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto source_digest = read_optional_digest(input, "regulatory network source");
    if (!source_digest) return core::Result<GeneRegulatoryNetwork>::failure(source_digest.error());
    if (auto result = require_record(input, "PROVENANCE"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto provenance = read_provenance(input);
    if (!provenance) return core::Result<GeneRegulatoryNetwork>::failure(provenance.error());

    GeneRegulatoryNetwork network;
    network.schema = schema.value();
    network.reference = reference.value();
    network.status = status.value();
    network.source_digest = source_digest.value();
    network.provenance = provenance.value();

    if (auto result = require_record(input, "ELEMENTS"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto element_count = read_uint(input, "regulatory element count");
    if (!element_count || element_count.value() > kMaxCollectionEntries) {
        return core::Result<GeneRegulatoryNetwork>::failure(parse_error(
            "invalid regulatory element count"));
    }
    for (std::uint64_t index = 0U; index < element_count.value(); ++index) {
        if (auto result = require_record(input, "ELEMENT"); !result) {
            return core::Result<GeneRegulatoryNetwork>::failure(result.error());
        }
        const auto id = read_uint(input, "regulatory element id");
        const auto gene = read_uint(input, "regulatory element gene");
        const auto name = read_string(input, "regulatory element name", 256U, true);
        const auto kind = read_string(input, "regulatory element kind", 128U, true);
        const auto source = read_string(input, "regulatory element source reference",
                                         kMaxReferenceBytes, true);
        const auto element_provenance = read_provenance(input);
        if (!id || !gene || !name || !kind || !source || !element_provenance) {
            const auto& error = !id ? id.error() : !gene ? gene.error() : !name ? name.error() :
                !kind ? kind.error() : !source ? source.error() : element_provenance.error();
            return core::Result<GeneRegulatoryNetwork>::failure(error);
        }
        const auto inserted = network.insert_element(RegulatoryElement{
            RegulatoryElementId{id.value()}, GeneId{gene.value()}, name.value(), kind.value(),
            source.value(), element_provenance.value()});
        if (!inserted) return core::Result<GeneRegulatoryNetwork>::failure(inserted.error());
    }

    if (auto result = require_record(input, "EDGES"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto edge_count = read_uint(input, "regulatory edge count");
    if (!edge_count || edge_count.value() > kMaxCollectionEntries) {
        return core::Result<GeneRegulatoryNetwork>::failure(parse_error(
            "invalid regulatory edge count"));
    }
    for (std::uint64_t index = 0U; index < edge_count.value(); ++index) {
        if (auto result = require_record(input, "EDGE"); !result) {
            return core::Result<GeneRegulatoryNetwork>::failure(result.error());
        }
        const auto id = read_uint(input, "regulatory edge id");
        const auto source_gene = read_uint(input, "regulatory edge source gene");
        const auto target_gene = read_uint(input, "regulatory edge target gene");
        const auto element = read_uint(input, "regulatory edge element");
        const auto mode = read_string(input, "regulatory edge mode", 128U, true);
        const auto condition = read_string(input, "regulatory edge condition", kMaxTextBytes);
        const auto stage = read_optional_id<StageId>(input, "regulatory edge stage");
        const auto edge_provenance = read_provenance(input);
        if (!id || !source_gene || !target_gene || !element || !mode || !condition || !stage ||
            !edge_provenance) {
            const auto& error = !id ? id.error() : !source_gene ? source_gene.error() :
                !target_gene ? target_gene.error() : !element ? element.error() :
                !mode ? mode.error() : !condition ? condition.error() : !stage ? stage.error() :
                edge_provenance.error();
            return core::Result<GeneRegulatoryNetwork>::failure(error);
        }
        const auto inserted = network.insert_edge(RegulatoryEdge{
            RegulatoryEdgeId{id.value()}, GeneId{source_gene.value()}, GeneId{target_gene.value()},
            RegulatoryElementId{element.value()}, mode.value(), condition.value(), stage.value(),
            edge_provenance.value()});
        if (!inserted) return core::Result<GeneRegulatoryNetwork>::failure(inserted.error());
    }

    if (auto result = require_record(input, "EXPRESSIONS"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    const auto expression_count = read_uint(input, "spatial expression count");
    if (!expression_count || expression_count.value() > kMaxCollectionEntries) {
        return core::Result<GeneRegulatoryNetwork>::failure(parse_error(
            "invalid spatial expression count"));
    }
    for (std::uint64_t index = 0U; index < expression_count.value(); ++index) {
        if (auto result = require_record(input, "EXPRESSION"); !result) {
            return core::Result<GeneRegulatoryNetwork>::failure(result.error());
        }
        const auto id = read_uint(input, "spatial expression id");
        const auto gene = read_uint(input, "spatial expression gene");
        const auto structure = read_optional_id<StructureId>(input, "spatial expression structure");
        const auto region = read_optional_id<RegionId>(input, "spatial expression region");
        const auto field = read_optional_id<FieldId>(input, "spatial expression field");
        const auto stage = read_optional_id<StageId>(input, "spatial expression stage");
        const auto mode = read_string(input, "spatial expression mode", 128U, true);
        const auto value_present = read_uint(input, "spatial expression value presence");
        if (!id || !gene || !structure || !region || !field || !stage || !mode ||
            !value_present || value_present.value() > 1U) {
            return core::Result<GeneRegulatoryNetwork>::failure(parse_error(
                "invalid spatial expression header"));
        }
        std::optional<double> value;
        if (value_present.value() != 0U) {
            const auto parsed = read_double(input, "spatial expression value");
            if (!parsed) return core::Result<GeneRegulatoryNetwork>::failure(parsed.error());
            value = parsed.value();
        }
        const auto categorical = read_string(input, "spatial expression categorical state", 128U);
        const auto units = read_string(input, "spatial expression units", 128U);
        const auto expression_provenance = read_provenance(input);
        if (!categorical || !units || !expression_provenance) {
            const auto& error = !categorical ? categorical.error() : !units ? units.error() :
                expression_provenance.error();
            return core::Result<GeneRegulatoryNetwork>::failure(error);
        }
        const auto inserted = network.insert_expression(SpatialExpression{
            ExpressionId{id.value()}, GeneId{gene.value()}, structure.value(), region.value(),
            field.value(), stage.value(), mode.value(), value, categorical.value(), units.value(),
            expression_provenance.value()});
        if (!inserted) return core::Result<GeneRegulatoryNetwork>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<GeneRegulatoryNetwork>::failure(
            parse_error("gene regulatory network contains trailing data"));
    }
    if (auto result = network.validate(); !result) {
        return core::Result<GeneRegulatoryNetwork>::failure(result.error());
    }
    return core::Result<GeneRegulatoryNetwork>::success(std::move(network));
}

namespace {

std::string developmental_stage_fingerprint(const DevelopmentStage& stage) {
    std::ostringstream output;
    output << stage.ordinal << ' ' << std::quoted(stage.name) << ' '
           << std::quoted(stage.condition) << ' ';
    write_provenance(output, stage.provenance);
    return output.str();
}

std::string developmental_instruction_fingerprint(
    const DevelopmentInstruction& instruction) {
    std::ostringstream output;
    output << instruction.stage.value << ' ' << std::quoted(instruction.opcode) << ' '
           << std::quoted(instruction.module) << ' ' << std::quoted(instruction.condition) << ' ';
    write_id_list(output, instruction.parameters);
    output << ' ';
    write_id_list(output, instruction.creates);
    output << ' ';
    write_id_list(output, instruction.modifies);
    output << ' ';
    write_provenance(output, instruction.provenance);
    return output.str();
}

} // namespace

DevelopmentalProgramDiff DevelopmentalProgram::diff_against(
    const DevelopmentalProgram& parent) const {
    DevelopmentalProgramDiff diff;
    diff.source_identity_changed = schema != parent.schema || reference != parent.reference ||
        source_digest != parent.source_digest;

    std::set<std::string> current_modules;
    std::set<std::string> parent_modules;
    std::set<std::string> changed_modules;
    for (const auto& [id, stage] : stages_) {
        const auto previous = parent.stages_.find(id);
        if (previous == parent.stages_.end()) {
            diff.added_stages.push_back(id);
        } else if (developmental_stage_fingerprint(stage) !=
                   developmental_stage_fingerprint(previous->second)) {
            diff.changed_stages.push_back(id);
        }
    }
    for (const auto& [id, stage] : parent.stages_) {
        static_cast<void>(stage);
        if (!stages_.contains(id)) diff.removed_stages.push_back(id);
    }

    for (const auto& [id, instruction] : instructions_) {
        current_modules.insert(instruction.module);
        const auto previous = parent.instructions_.find(id);
        if (previous == parent.instructions_.end()) {
            diff.added_instructions.push_back(id);
        } else if (developmental_instruction_fingerprint(instruction) !=
                   developmental_instruction_fingerprint(previous->second)) {
            diff.changed_instructions.push_back(id);
            changed_modules.insert(instruction.module);
            changed_modules.insert(previous->second.module);
        }
    }
    for (const auto& [id, instruction] : parent.instructions_) {
        parent_modules.insert(instruction.module);
        if (!instructions_.contains(id)) diff.removed_instructions.push_back(id);
    }
    std::set_difference(current_modules.begin(), current_modules.end(),
                        parent_modules.begin(), parent_modules.end(),
                        std::back_inserter(diff.added_modules));
    std::set_difference(parent_modules.begin(), parent_modules.end(),
                        current_modules.begin(), current_modules.end(),
                        std::back_inserter(diff.removed_modules));
    diff.changed_modules.assign(changed_modules.begin(), changed_modules.end());
    return diff;
}

core::Result<void> DevelopmentStage::validate() const {
    if (auto result = validate_id(id.value, "development stage id"); !result) return result;
    if (auto result = validate_text(name, "development stage name", 256U, true); !result) return result;
    if (auto result = validate_text(condition, "development stage condition", kMaxTextBytes);
        !result) return result;
    return provenance.validate();
}

core::Result<void> DevelopmentInstruction::validate() const {
    if (auto result = validate_id(id.value, "development instruction id"); !result) return result;
    if (auto result = validate_id(stage.value, "development instruction stage"); !result) return result;
    if (auto result = validate_text(opcode, "development instruction opcode", 128U, true);
        !result) return result;
    if (auto result = validate_text(module, "development instruction module", 256U, true);
        !result) return result;
    if (auto result = validate_text(condition, "development instruction condition", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_id_list(parameters, "development instruction parameters"); !result) {
        return result;
    }
    if (auto result = validate_id_list(creates, "development instruction creates"); !result) {
        return result;
    }
    if (auto result = validate_id_list(modifies, "development instruction modifies"); !result) {
        return result;
    }
    std::set<StructureId> created(creates.begin(), creates.end());
    for (const auto structure : modifies) {
        if (created.contains(structure)) {
            return core::Result<void>::failure(validation(
                "development instruction cannot both create and modify a structure"));
        }
    }
    return provenance.validate();
}

core::Result<void> DevelopmentalProgram::insert_stage(DevelopmentStage value) {
    return insert_unique(stages_, std::move(value), "development stage");
}

core::Result<void> DevelopmentalProgram::insert_instruction(DevelopmentInstruction value) {
    return insert_unique(instructions_, std::move(value), "development instruction");
}

core::Result<void> DevelopmentalProgram::validate() const {
    if (auto result = validate_text(schema, "development program schema", 128U, true); !result) {
        return result;
    }
    if (auto result = validate_text(reference, "development program reference", kMaxReferenceBytes, true);
        !result) return result;
    if (auto result = validate_text(description, "development program description", kMaxTextBytes);
        !result) return result;
    if (auto result = validate_digest(source_digest, "development program source digest"); !result) {
        return result;
    }
    if (stages_.size() > kMaxCollectionEntries || instructions_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "development program collection exceeds the safety limit"));
    }
    std::set<std::uint64_t> ordinals;
    for (const auto& [id, stage] : stages_) {
        static_cast<void>(id);
        if (auto result = stage.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("development stage"));
        }
        if (!ordinals.insert(stage.ordinal).second) {
            return core::Result<void>::failure(validation(
                "development program contains duplicate stage ordinals"));
        }
    }
    for (const auto& [id, instruction] : instructions_) {
        static_cast<void>(id);
        if (auto result = instruction.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("development instruction"));
        }
        if (auto result = validate_reference(stages_, instruction.stage,
                                             "development instruction stage"); !result) {
            return result;
        }
    }
    return core::Result<void>::success();
}

std::string DevelopmentalProgram::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_DEVELOPMENTAL_PROGRAM " << kSchemaVersion << '\n';
    output << std::quoted(schema) << ' ' << std::quoted(reference) << ' '
           << std::quoted(description);
    write_optional_digest(output, source_digest);
    output << '\n';
    output << "STAGES " << stages_.size() << '\n';
    for (const auto& [id, stage] : stages_) {
        output << "STAGE " << id.value << ' ' << stage.ordinal << ' '
               << std::quoted(stage.name) << ' ' << std::quoted(stage.condition) << ' ';
        write_provenance(output, stage.provenance);
        output << '\n';
    }
    output << "INSTRUCTIONS " << instructions_.size() << '\n';
    for (const auto& [id, instruction] : instructions_) {
        output << "INSTRUCTION " << id.value << ' ' << instruction.stage.value << ' '
               << std::quoted(instruction.opcode) << ' ' << std::quoted(instruction.module) << ' '
               << std::quoted(instruction.condition) << ' ';
        write_id_list(output, instruction.parameters);
        output << ' ';
        write_id_list(output, instruction.creates);
        output << ' ';
        write_id_list(output, instruction.modifies);
        output << ' ';
        write_provenance(output, instruction.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<DevelopmentalProgram> DevelopmentalProgram::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<DevelopmentalProgram>::failure(
            parse_error("development program record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_DEVELOPMENTAL_PROGRAM"); !result) {
        return core::Result<DevelopmentalProgram>::failure(result.error());
    }
    const auto version = read_uint(input, "development program schema version");
    if (!version || version.value() != kSchemaVersion) {
        return core::Result<DevelopmentalProgram>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported developmental program schema version"));
    }
    const auto schema = read_string(input, "development program schema", 128U, true);
    const auto reference = read_string(input, "development program reference", kMaxReferenceBytes, true);
    const auto description = read_string(input, "development program description", kMaxTextBytes);
    const auto source_digest = read_optional_digest(input, "development program source");
    if (!schema || !reference || !description || !source_digest) {
        return core::Result<DevelopmentalProgram>::failure(parse_error(
            "invalid developmental program header"));
    }
    DevelopmentalProgram program;
    program.schema = schema.value();
    program.reference = reference.value();
    program.description = description.value();
    program.source_digest = source_digest.value();
    if (auto result = require_record(input, "STAGES"); !result) {
        return core::Result<DevelopmentalProgram>::failure(result.error());
    }
    const auto stage_count = read_uint(input, "development stage count");
    if (!stage_count || stage_count.value() > kMaxCollectionEntries) {
        return core::Result<DevelopmentalProgram>::failure(parse_error(
            "invalid development stage count"));
    }
    for (std::uint64_t index = 0U; index < stage_count.value(); ++index) {
        if (auto result = require_record(input, "STAGE"); !result) {
            return core::Result<DevelopmentalProgram>::failure(result.error());
        }
        const auto id = read_uint(input, "development stage id");
        const auto ordinal = read_uint(input, "development stage ordinal");
        const auto name = read_string(input, "development stage name", 256U, true);
        const auto condition = read_string(input, "development stage condition", kMaxTextBytes);
        const auto provenance = read_provenance(input);
        if (!id || !ordinal || !name || !condition || !provenance) {
            return core::Result<DevelopmentalProgram>::failure(parse_error(
                "invalid development stage record"));
        }
        const auto inserted = program.insert_stage(DevelopmentStage{
            StageId{id.value()}, ordinal.value(), name.value(), condition.value(), provenance.value()});
        if (!inserted) {
            return core::Result<DevelopmentalProgram>::failure(inserted.error());
        }
    }
    if (auto result = require_record(input, "INSTRUCTIONS"); !result) {
        return core::Result<DevelopmentalProgram>::failure(result.error());
    }
    const auto instruction_count = read_uint(input, "development instruction count");
    if (!instruction_count || instruction_count.value() > kMaxCollectionEntries) {
        return core::Result<DevelopmentalProgram>::failure(parse_error(
            "invalid development instruction count"));
    }
    for (std::uint64_t index = 0U; index < instruction_count.value(); ++index) {
        if (auto result = require_record(input, "INSTRUCTION"); !result) {
            return core::Result<DevelopmentalProgram>::failure(result.error());
        }
        const auto id = read_uint(input, "development instruction id");
        const auto stage = read_uint(input, "development instruction stage");
        const auto opcode = read_string(input, "development instruction opcode", 128U, true);
        const auto module = read_string(input, "development instruction module", 256U, true);
        const auto condition = read_string(input, "development instruction condition", kMaxTextBytes);
        const auto parameters = read_id_list<ParameterId>(input, "development instruction parameters");
        const auto creates = read_id_list<StructureId>(input, "development instruction creates");
        const auto modifies = read_id_list<StructureId>(input, "development instruction modifies");
        const auto provenance = read_provenance(input);
        if (!id || !stage || !opcode || !module || !condition || !parameters || !creates ||
            !modifies || !provenance) {
            return core::Result<DevelopmentalProgram>::failure(parse_error(
                "invalid development instruction record"));
        }
        const auto inserted = program.insert_instruction(DevelopmentInstruction{
            InstructionId{id.value()}, StageId{stage.value()}, opcode.value(), module.value(),
            condition.value(), parameters.value(), creates.value(), modifies.value(), provenance.value()});
        if (!inserted) {
            return core::Result<DevelopmentalProgram>::failure(inserted.error());
        }
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<DevelopmentalProgram>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<DevelopmentalProgram>::failure(
            parse_error("development program record contains trailing data"));
    }
    if (auto result = program.validate(); !result) {
        return core::Result<DevelopmentalProgram>::failure(result.error());
    }
    return core::Result<DevelopmentalProgram>::success(std::move(program));
}

core::Result<void> DevelopmentTraceEntry::validate() const {
    if (auto result = validate_id(id.value, "development trace entry id"); !result) return result;
    if (auto result = validate_id(stage.value, "development trace entry stage"); !result) return result;
    if (auto result = validate_id(instruction.value, "development trace entry instruction"); !result) {
        return result;
    }
    if (auto result = validate_id_list(created, "development trace created structures"); !result) {
        return result;
    }
    if (auto result = validate_id_list(modified, "development trace modified structures"); !result) {
        return result;
    }
    if (tissue_region.has_value() && !tissue_region->value) {
        return core::Result<void>::failure(validation(
            "development trace tissue region must be non-zero"));
    }
    if (parameter_values.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "development trace parameter values exceed the safety limit"));
    }
    for (const auto value : parameter_values) {
        if (!std::isfinite(value)) {
            return core::Result<void>::failure(validation(
                "development trace parameter value is not finite"));
        }
    }
    if (auto result = validate_digest(result_digest, "development trace result digest");
        !result) {
        return result;
    }
    if (auto result = validate_text(evidence, "development trace evidence", kMaxTextBytes, true);
        !result) return result;
    return provenance.validate();
}

core::Result<void> DevelopmentalTrace::insert_entry(DevelopmentTraceEntry value) {
    return insert_unique(entries_, std::move(value), "development trace entry");
}

core::Result<void> DevelopmentalTrace::validate() const {
    if (auto result = validate_text(program_reference, "development trace program reference",
                                    kMaxReferenceBytes, true); !result) return result;
    if (auto result = validate_text(genome_reference, "development trace genome reference",
                                    kMaxReferenceBytes); !result) return result;
    if (auto result = validate_digest(program_digest, "development trace program digest");
        !result) {
        return result;
    }
    if (auto result = validate_digest(genome_digest, "development trace genome digest");
        !result) {
        return result;
    }
    if (entries_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "development trace collection exceeds the safety limit"));
    }
    for (const auto& [id, entry] : entries_) {
        static_cast<void>(id);
        if (auto result = entry.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("development trace entry"));
        }
    }
    return core::Result<void>::success();
}

std::string DevelopmentalTrace::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_DEVELOPMENTAL_TRACE " << kSchemaVersion << '\n';
    output << std::quoted(program_reference) << ' ' << std::quoted(genome_reference);
    write_optional_digest(output, program_digest);
    write_optional_digest(output, genome_digest);
    output << '\n';
    output << "ENTRIES " << entries_.size() << '\n';
    for (const auto& [id, entry] : entries_) {
        output << "TRACE " << id.value << ' ' << entry.stage.value << ' '
               << entry.instruction.value << ' ';
        write_id_list(output, entry.created);
        output << ' ';
        write_id_list(output, entry.modified);
        output << ' ' << std::quoted(entry.evidence) << ' ';
        write_provenance(output, entry.provenance);
        output << ' ' << optional_id(entry.tissue_region) << ' ';
        write_double_list(output, entry.parameter_values);
        output << ' ' << (entry.seed.has_value() ? 1U : 0U);
        if (entry.seed.has_value()) output << ' ' << *entry.seed;
        write_optional_digest(output, entry.result_digest);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<DevelopmentalTrace> DevelopmentalTrace::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<DevelopmentalTrace>::failure(
            parse_error("development trace record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_DEVELOPMENTAL_TRACE"); !result) {
        return core::Result<DevelopmentalTrace>::failure(result.error());
    }
    const auto version = read_uint(input, "development trace schema version");
    if (!version || (version.value() != 1U && version.value() != 2U &&
                     version.value() != kSchemaVersion)) {
        return core::Result<DevelopmentalTrace>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported developmental trace schema version"));
    }
    const auto program = read_string(input, "development trace program reference", kMaxReferenceBytes, true);
    const auto genome = read_string(input, "development trace genome reference", kMaxReferenceBytes);
    if (!program || !genome) {
        return core::Result<DevelopmentalTrace>::failure(parse_error(
            "invalid developmental trace header"));
    }
    DevelopmentalTrace trace;
    trace.program_reference = program.value();
    trace.genome_reference = genome.value();
    if (version.value() >= 3U) {
        const auto program_digest = read_optional_digest(input, "development trace program");
        const auto genome_digest = read_optional_digest(input, "development trace genome");
        if (!program_digest || !genome_digest) {
            return core::Result<DevelopmentalTrace>::failure(parse_error(
                "invalid developmental trace source digest metadata"));
        }
        trace.program_digest = program_digest.value();
        trace.genome_digest = genome_digest.value();
    }
    if (auto result = require_record(input, "ENTRIES"); !result) {
        return core::Result<DevelopmentalTrace>::failure(result.error());
    }
    const auto count = read_uint(input, "development trace entry count");
    if (!count || count.value() > kMaxCollectionEntries) {
        return core::Result<DevelopmentalTrace>::failure(parse_error(
            "invalid development trace entry count"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        if (auto result = require_record(input, "TRACE"); !result) {
            return core::Result<DevelopmentalTrace>::failure(result.error());
        }
        const auto id = read_uint(input, "development trace entry id");
        const auto stage = read_uint(input, "development trace entry stage");
        const auto instruction = read_uint(input, "development trace entry instruction");
        const auto created = read_id_list<StructureId>(input, "development trace created structures");
        const auto modified = read_id_list<StructureId>(input, "development trace modified structures");
        const auto evidence = read_string(input, "development trace evidence", kMaxTextBytes, true);
        const auto provenance = read_provenance(input);
        std::optional<TissueRegionId> tissue_region;
        std::vector<double> parameter_values;
        std::optional<std::uint64_t> seed;
        std::optional<assets::Sha256Digest> result_digest;
        if (version.value() >= 2U) {
            const auto parsed_tissue_region = read_optional_id<TissueRegionId>(
                input, "development trace tissue region");
            const auto parsed_parameter_values = read_double_list(
                input, "development trace parameter values");
            if (!parsed_tissue_region || !parsed_parameter_values) {
                return core::Result<DevelopmentalTrace>::failure(parse_error(
                    "invalid development trace evidence metadata"));
            }
            tissue_region = parsed_tissue_region.value();
            parameter_values = parsed_parameter_values.value();
        }
        if (version.value() >= 3U) {
            const auto seed_present = read_uint(input, "development trace seed presence");
            if (!seed_present || seed_present.value() > 1U) {
                return core::Result<DevelopmentalTrace>::failure(parse_error(
                    "invalid development trace seed presence"));
            }
            if (seed_present.value() != 0U) {
                const auto parsed_seed = read_uint(input, "development trace seed");
                if (!parsed_seed) {
                    return core::Result<DevelopmentalTrace>::failure(parsed_seed.error());
                }
                seed = parsed_seed.value();
            }
            const auto parsed_result_digest = read_optional_digest(
                input, "development trace result");
            if (!parsed_result_digest) {
                return core::Result<DevelopmentalTrace>::failure(parsed_result_digest.error());
            }
            result_digest = parsed_result_digest.value();
        }
        if (!id || !stage || !instruction || !created || !modified || !evidence || !provenance) {
            return core::Result<DevelopmentalTrace>::failure(parse_error(
                "invalid development trace entry"));
        }
        const auto inserted = trace.insert_entry(DevelopmentTraceEntry{
            TraceId{id.value()}, StageId{stage.value()}, InstructionId{instruction.value()},
            created.value(), modified.value(), evidence.value(), provenance.value(), tissue_region,
            std::move(parameter_values), seed, result_digest});
        if (!inserted) return core::Result<DevelopmentalTrace>::failure(inserted.error());
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<DevelopmentalTrace>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<DevelopmentalTrace>::failure(
            parse_error("development trace record contains trailing data"));
    }
    if (auto result = trace.validate(); !result) {
        return core::Result<DevelopmentalTrace>::failure(result.error());
    }
    return core::Result<DevelopmentalTrace>::success(std::move(trace));
}

constexpr std::size_t kMaxLineageNewickBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxLineageNewickLabelBytes = 64U * 1024U;
constexpr std::size_t kMaxLineageNewickDepth = 4096U;

std::uint64_t stable_lineage_newick_id(std::string_view key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : key) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

bool is_newick_delimiter(char character) {
    return character == '(' || character == ')' || character == ',' || character == ';' ||
        character == ':' || character == '[' || character == ']' || character == '#';
}

core::Result<void> validate_newick_label(std::string_view value, std::string_view field) {
    if (value.size() > kMaxLineageNewickLabelBytes) {
        return core::Result<void>::failure(validation(
            std::string(field) + " exceeds the safety limit"));
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x20U || byte == 0x7fU || is_newick_delimiter(character) ||
            character == '\'' || character == '"' || character == '\\') {
            return core::Result<void>::failure(validation(
                std::string(field) + " contains an unsupported Newick delimiter or control byte"));
        }
    }
    return core::Result<void>::success();
}

struct NewickNodeSpec {
    std::string label;
    std::vector<NewickNodeSpec> children;
};

class NewickParser {
public:
    explicit NewickParser(std::string_view text) : text_(text) {}

    core::Result<NewickNodeSpec> parse() {
        if (text_.empty() || text_.size() > kMaxLineageNewickBytes) {
            return core::Result<NewickNodeSpec>::failure(parse_error(
                "Newick tree is empty or exceeds the safety limit"));
        }
        for (const auto character : text_) {
            if (static_cast<unsigned char>(character) <= 0x20U || character == 0x7fU) {
                return core::Result<NewickNodeSpec>::failure(parse_error(
                    "Newick tree must not contain whitespace or control bytes"));
            }
        }
        auto root = parse_node(0U);
        if (!root) return root;
        if (position_ >= text_.size() || text_[position_] != ';') {
            return core::Result<NewickNodeSpec>::failure(parse_error(
                "Newick tree must terminate with a semicolon"));
        }
        ++position_;
        if (position_ != text_.size()) {
            return core::Result<NewickNodeSpec>::failure(parse_error(
                "Newick tree contains trailing data"));
        }
        return root;
    }

private:
    core::Result<NewickNodeSpec> parse_node(std::size_t depth) {
        if (depth > kMaxLineageNewickDepth) {
            return core::Result<NewickNodeSpec>::failure(parse_error(
                "Newick tree exceeds the depth safety limit"));
        }
        NewickNodeSpec node;
        if (position_ < text_.size() && text_[position_] == '(') {
            ++position_;
            if (position_ >= text_.size() || text_[position_] == ')' ||
                text_[position_] == ',') {
                return core::Result<NewickNodeSpec>::failure(parse_error(
                    "Newick internal node must contain at least one child"));
            }
            while (true) {
                auto child = parse_node(depth + 1U);
                if (!child) return child;
                node.children.push_back(std::move(child.value()));
                if (node.children.size() > kMaxCollectionEntries) {
                    return core::Result<NewickNodeSpec>::failure(parse_error(
                        "Newick tree exceeds the node safety limit"));
                }
                if (position_ >= text_.size()) {
                    return core::Result<NewickNodeSpec>::failure(parse_error(
                        "Newick internal node is missing a closing parenthesis"));
                }
                if (text_[position_] == ',') {
                    ++position_;
                    if (position_ >= text_.size() || text_[position_] == ')' ||
                        text_[position_] == ',') {
                        return core::Result<NewickNodeSpec>::failure(parse_error(
                            "Newick child list contains an empty member"));
                    }
                    continue;
                }
                if (text_[position_] != ')') {
                    return core::Result<NewickNodeSpec>::failure(parse_error(
                        "Newick child list must use commas and a closing parenthesis"));
                }
                ++position_;
                break;
            }
            if (position_ < text_.size() && !is_newick_delimiter(text_[position_])) {
                const auto label = read_label();
                if (!label) return core::Result<NewickNodeSpec>::failure(label.error());
                node.label = label.value();
            }
        } else {
            if (position_ >= text_.size() || is_newick_delimiter(text_[position_])) {
                return core::Result<NewickNodeSpec>::failure(parse_error(
                    "Newick leaf is missing a label"));
            }
            const auto label = read_label();
            if (!label) return core::Result<NewickNodeSpec>::failure(label.error());
            node.label = label.value();
            if (node.label.empty()) {
                return core::Result<NewickNodeSpec>::failure(parse_error(
                    "Newick leaf is missing a label"));
            }
        }
        if (position_ < text_.size() && text_[position_] == ':') {
            return core::Result<NewickNodeSpec>::failure(validation(
                "Newick branch lengths are unsupported and must not be discarded"));
        }
        if (position_ < text_.size() &&
            (text_[position_] == '[' || text_[position_] == ']')) {
            return core::Result<NewickNodeSpec>::failure(validation(
                "Newick comments are unsupported and must not be discarded"));
        }
        return core::Result<NewickNodeSpec>::success(std::move(node));
    }

    core::Result<std::string> read_label() {
        const auto start = position_;
        while (position_ < text_.size() && !is_newick_delimiter(text_[position_])) {
            ++position_;
        }
        const std::string_view label = text_.substr(start, position_ - start);
        if (auto result = validate_newick_label(label, "Newick label"); !result) {
            return core::Result<std::string>::failure(result.error());
        }
        return core::Result<std::string>::success(std::string(label));
    }

    std::string_view text_;
    std::size_t position_ = 0U;
};

core::Result<std::uint64_t> parse_newick_uint(
    std::string_view token,
    std::string_view field) {
    if (token.empty()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(field) + " is empty"));
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(field) + " is not an unsigned integer"));
    }
    return core::Result<std::uint64_t>::success(value);
}

core::Result<void> LineageNode::validate() const {
    if (auto result = validate_id(id.value, "lineage node id"); !result) return result;
    if (parent_a.has_value() && (!*parent_a || *parent_a == id)) {
        return core::Result<void>::failure(validation(
            "lineage node first parent is invalid"));
    }
    if (parent_b.has_value() && (!*parent_b || *parent_b == id)) {
        return core::Result<void>::failure(validation(
            "lineage node second parent is invalid"));
    }
    if (parent_a.has_value() && parent_b.has_value() && *parent_a == *parent_b) {
        return core::Result<void>::failure(validation(
            "lineage node parents must be distinct"));
    }
    if (!parent_a.has_value() && !parent_b.has_value() && generation != 0U) {
        return core::Result<void>::failure(validation(
            "lineage root node must have generation zero"));
    }
    if (auto result = validate_digest(genome_digest, "lineage genome digest"); !result) return result;
    if (auto result = validate_digest(morphology_digest, "lineage morphology digest"); !result) return result;
    if (auto result = validate_text(source_identifier, "lineage source identifier",
                                    kMaxReferenceBytes); !result) {
        return result;
    }
    return provenance.validate();
}

core::Result<void> MutationReceipt::validate() const {
    if (auto result = validate_id(id.value, "mutation receipt id"); !result) return result;
    if (auto result = validate_id(parent_lineage.value, "mutation parent lineage"); !result) return result;
    if (auto result = validate_id(child_lineage.value, "mutation child lineage"); !result) return result;
    if (parent_lineage == child_lineage) {
        return core::Result<void>::failure(validation(
            "mutation receipt parent and child lineages must differ"));
    }
    if (auto result = validate_digest(parent_genome_digest, "mutation parent genome digest"); !result) {
        return result;
    }
    if (auto result = validate_digest(child_genome_digest, "mutation child genome digest"); !result) {
        return result;
    }
    if (auto result = validate_text(operator_name, "mutation operator", 256U, true); !result) {
        return result;
    }
    if (auto result = validate_id_list(changed_genes, "mutation changed genes"); !result) {
        return result;
    }
    if (auto result = validate_id_list(changed_instructions,
                                       "mutation changed instructions"); !result) {
        return result;
    }
    if (changed_neural_values.size() > kMaxTags) {
        return core::Result<void>::failure(validation(
            "mutation changed neural values exceed the safety limit"));
    }
    {
        std::set<std::string> unique_values;
        for (const auto& value : changed_neural_values) {
            if (auto result = validate_text(value, "mutation changed neural value",
                                             kMaxTextBytes, true); !result) {
                return result;
            }
            if (!unique_values.insert(value).second) {
                return core::Result<void>::failure(validation(
                    "mutation changed neural values contain a duplicate"));
            }
        }
    }
    if (auto result = validate_text(outcome, "mutation outcome", kMaxTextBytes, true); !result) {
        return result;
    }
    return provenance.validate();
}

core::Result<void> RecombinationReceipt::validate() const {
    if (auto result = validate_id(id.value, "recombination receipt id"); !result) return result;
    if (auto result = validate_id(parent_a.value, "recombination first parent"); !result) {
        return result;
    }
    if (auto result = validate_id(parent_b.value, "recombination second parent"); !result) {
        return result;
    }
    if (auto result = validate_id(child_lineage.value, "recombination child lineage"); !result) {
        return result;
    }
    if (parent_a == parent_b || parent_a == child_lineage || parent_b == child_lineage) {
        return core::Result<void>::failure(validation(
            "recombination parents and child must be distinct lineages"));
    }
    if (auto result = validate_digest(child_genome_digest,
                                      "recombination child genome digest"); !result) {
        return result;
    }
    if (module_origins.empty() || module_origins.size() > kMaxTags) {
        return core::Result<void>::failure(validation(
            "recombination module origins must be bounded and non-empty"));
    }
    std::set<std::string> unique_origins;
    for (const auto& origin : module_origins) {
        if (auto result = validate_text(origin, "recombination module origin",
                                        kMaxReferenceBytes, true); !result) {
            return result;
        }
        if (!unique_origins.insert(origin).second) {
            return core::Result<void>::failure(validation(
                "recombination module origins contain a duplicate"));
        }
    }
    if (post_mutation.has_value() && !post_mutation->value) {
        return core::Result<void>::failure(validation(
            "recombination post-mutation id must be non-zero"));
    }
    if (auto result = validate_text(outcome, "recombination outcome", kMaxTextBytes, true);
        !result) {
        return result;
    }
    return provenance.validate();
}

core::Result<void> LineageGraph::insert_node(LineageNode value) {
    return insert_unique(nodes_, std::move(value), "lineage node");
}

core::Result<void> LineageGraph::insert_mutation(MutationReceipt value) {
    return insert_unique(mutations_, std::move(value), "mutation receipt");
}

core::Result<void> LineageGraph::insert_recombination(RecombinationReceipt value) {
    return insert_unique(recombinations_, std::move(value), "recombination receipt");
}

core::Result<void> LineageGraph::validate() const {
    if (nodes_.size() > kMaxCollectionEntries || mutations_.size() > kMaxCollectionEntries ||
        recombinations_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(
            "lineage graph collection exceeds the safety limit"));
    }
    for (const auto& [id, node] : nodes_) {
        static_cast<void>(id);
        if (auto result = node.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("lineage node"));
        }
        if (auto result = validate_optional_reference(node.parent_a, "lineage node first parent", nodes_);
            !result) return result;
        if (auto result = validate_optional_reference(node.parent_b, "lineage node second parent", nodes_);
            !result) return result;
        if (node.parent_a.has_value() && node.generation <= nodes_.at(*node.parent_a).generation) {
            return core::Result<void>::failure(validation(
                "lineage child generation must be greater than its first parent"));
        }
        if (node.parent_b.has_value() && node.generation <= nodes_.at(*node.parent_b).generation) {
            return core::Result<void>::failure(validation(
                "lineage child generation must be greater than its second parent"));
        }
    }
    std::map<LineageId, std::uint8_t> visit;
    const auto visit_node = [&](const auto& self, LineageId id) -> core::Result<void> {
        const auto state = visit[id];
        if (state == 1U) {
            return core::Result<void>::failure(validation("lineage graph contains a cycle"));
        }
        if (state == 2U) return core::Result<void>::success();
        visit[id] = 1U;
        const auto& node = nodes_.at(id);
        if (node.parent_a.has_value()) {
            if (auto result = self(self, *node.parent_a); !result) return result;
        }
        if (node.parent_b.has_value()) {
            if (auto result = self(self, *node.parent_b); !result) return result;
        }
        visit[id] = 2U;
        return core::Result<void>::success();
    };
    for (const auto& [id, node] : nodes_) {
        static_cast<void>(node);
        if (auto result = visit_node(visit_node, id); !result) return result;
    }
    for (const auto& [id, mutation] : mutations_) {
        static_cast<void>(id);
        if (auto result = mutation.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("mutation receipt"));
        }
        if (auto result = validate_reference(nodes_, mutation.parent_lineage,
                                             "mutation parent lineage"); !result) return result;
        if (auto result = validate_reference(nodes_, mutation.child_lineage,
                                             "mutation child lineage"); !result) return result;
        const auto& child = nodes_.at(mutation.child_lineage);
        if ((!child.parent_a.has_value() || *child.parent_a != mutation.parent_lineage) &&
            (!child.parent_b.has_value() || *child.parent_b != mutation.parent_lineage)) {
            return core::Result<void>::failure(validation(
                "mutation receipt parent is not a parent of its child lineage"));
        }
    }
    for (const auto& [id, recombination] : recombinations_) {
        static_cast<void>(id);
        if (auto result = recombination.validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("recombination receipt"));
        }
        if (auto result = validate_reference(nodes_, recombination.parent_a,
                                             "recombination first parent"); !result) return result;
        if (auto result = validate_reference(nodes_, recombination.parent_b,
                                             "recombination second parent"); !result) return result;
        if (auto result = validate_reference(nodes_, recombination.child_lineage,
                                             "recombination child lineage"); !result) return result;
        const auto& child = nodes_.at(recombination.child_lineage);
        const auto parents_match =
            child.parent_a.has_value() && child.parent_b.has_value() &&
            ((*child.parent_a == recombination.parent_a && *child.parent_b == recombination.parent_b) ||
             (*child.parent_a == recombination.parent_b && *child.parent_b == recombination.parent_a));
        if (!parents_match) {
            return core::Result<void>::failure(validation(
                "recombination parents are not the two parents of its child lineage"));
        }
        if (recombination.post_mutation.has_value()) {
            if (auto result = validate_reference(mutations_, *recombination.post_mutation,
                                                 "recombination post mutation"); !result) return result;
            if (mutations_.at(*recombination.post_mutation).parent_lineage !=
                recombination.child_lineage) {
                return core::Result<void>::failure(validation(
                    "recombination post mutation must start at the recombination child"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string LineageGraph::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_LINEAGE_GRAPH " << kSchemaVersion << '\n';
    output << "NODES " << nodes_.size() << '\n';
    for (const auto& [id, node] : nodes_) {
        output << "NODE " << id.value << ' ' << optional_id(node.parent_a) << ' '
               << optional_id(node.parent_b) << ' ' << node.generation;
        write_optional_digest(output, node.genome_digest);
        write_optional_digest(output, node.morphology_digest);
        output << ' ' << std::quoted(node.source_identifier) << ' ';
        write_provenance(output, node.provenance);
        output << '\n';
    }
    output << "MUTATIONS " << mutations_.size() << '\n';
    for (const auto& [id, mutation] : mutations_) {
        output << "MUTATION " << id.value << ' ' << mutation.parent_lineage.value << ' '
               << mutation.child_lineage.value;
        write_optional_digest(output, mutation.parent_genome_digest);
        write_optional_digest(output, mutation.child_genome_digest);
        output << ' ' << mutation.seed << ' ' << std::quoted(mutation.operator_name) << ' ';
        write_id_list(output, mutation.changed_genes);
        output << ' ';
        write_id_list(output, mutation.changed_instructions);
        output << ' ' << mutation.changed_neural_values.size();
        for (const auto& value : mutation.changed_neural_values) {
            output << ' ' << std::quoted(value);
        }
        output << ' ' << std::quoted(mutation.outcome) << ' ';
        write_provenance(output, mutation.provenance);
        output << '\n';
    }
    output << "RECOMBINATIONS " << recombinations_.size() << '\n';
    for (const auto& [id, recombination] : recombinations_) {
        output << "RECOMBINATION " << id.value << ' ' << recombination.parent_a.value << ' '
               << recombination.parent_b.value << ' ' << recombination.child_lineage.value;
        write_optional_digest(output, recombination.child_genome_digest);
        output << ' ' << optional_id(recombination.post_mutation) << ' '
               << recombination.module_origins.size();
        for (const auto& origin : recombination.module_origins) {
            output << ' ' << std::quoted(origin);
        }
        output << ' ' << std::quoted(recombination.outcome) << ' ';
        write_provenance(output, recombination.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<LineageGraph> LineageGraph::deserialize(std::string_view text) {
    if (text.size() > 4U * 1024U * 1024U) {
        return core::Result<LineageGraph>::failure(parse_error("lineage graph record is too large"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_LINEAGE_GRAPH"); !result) {
        return core::Result<LineageGraph>::failure(result.error());
    }
    const auto version = read_uint(input, "lineage graph schema version");
    if (!version || (version.value() != 1U && version.value() != 2U &&
                     version.value() != kSchemaVersion)) {
        return core::Result<LineageGraph>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported lineage graph schema version"));
    }
    LineageGraph graph;
    if (auto result = require_record(input, "NODES"); !result) {
        return core::Result<LineageGraph>::failure(result.error());
    }
    const auto node_count = read_uint(input, "lineage node count");
    if (!node_count || node_count.value() > kMaxCollectionEntries) {
        return core::Result<LineageGraph>::failure(parse_error("invalid lineage node count"));
    }
    for (std::uint64_t index = 0U; index < node_count.value(); ++index) {
        if (auto result = require_record(input, "NODE"); !result) {
            return core::Result<LineageGraph>::failure(result.error());
        }
        const auto id = read_uint(input, "lineage node id");
        const auto parent_a = read_optional_id<LineageId>(input, "lineage first parent");
        const auto parent_b = read_optional_id<LineageId>(input, "lineage second parent");
        const auto generation = read_uint(input, "lineage generation");
        const auto genome_digest = read_optional_digest(input, "lineage genome");
        const auto morphology_digest = read_optional_digest(input, "lineage morphology");
        std::string source_identifier;
        if (version.value() >= 3U) {
            const auto parsed_source_identifier = read_string(
                input, "lineage source identifier", kMaxReferenceBytes);
            if (!parsed_source_identifier) {
                return core::Result<LineageGraph>::failure(parsed_source_identifier.error());
            }
            source_identifier = parsed_source_identifier.value();
        }
        const auto provenance = read_provenance(input);
        if (!id || !parent_a || !parent_b || !generation || !genome_digest || !morphology_digest ||
            !provenance) {
            return core::Result<LineageGraph>::failure(parse_error("invalid lineage node record"));
        }
        const auto inserted = graph.insert_node(LineageNode{
            LineageId{id.value()}, parent_a.value(), parent_b.value(), generation.value(),
            genome_digest.value(), morphology_digest.value(), provenance.value(),
            std::move(source_identifier)});
        if (!inserted) return core::Result<LineageGraph>::failure(inserted.error());
    }
    if (auto result = require_record(input, "MUTATIONS"); !result) {
        return core::Result<LineageGraph>::failure(result.error());
    }
    const auto mutation_count = read_uint(input, "mutation receipt count");
    if (!mutation_count || mutation_count.value() > kMaxCollectionEntries) {
        return core::Result<LineageGraph>::failure(parse_error("invalid mutation receipt count"));
    }
    for (std::uint64_t index = 0U; index < mutation_count.value(); ++index) {
        if (auto result = require_record(input, "MUTATION"); !result) {
            return core::Result<LineageGraph>::failure(result.error());
        }
        const auto id = read_uint(input, "mutation receipt id");
        const auto parent = read_uint(input, "mutation parent lineage");
        const auto child = read_uint(input, "mutation child lineage");
        const auto parent_digest = read_optional_digest(input, "mutation parent genome");
        const auto child_digest = read_optional_digest(input, "mutation child genome");
        const auto seed = read_uint(input, "mutation seed");
        const auto operation = read_string(input, "mutation operator", 256U, true);
        const auto genes = read_id_list<GeneId>(input, "mutation changed genes");
        std::vector<InstructionId> instructions;
        std::vector<std::string> neural_values;
        if (version.value() >= 2U) {
            const auto parsed_instructions = read_id_list<InstructionId>(
                input, "mutation changed instructions");
            const auto neural_count = read_uint(input, "mutation changed neural value count");
            if (!parsed_instructions || !neural_count || neural_count.value() > kMaxTags) {
                return core::Result<LineageGraph>::failure(parse_error(
                    "invalid mutation changed instruction/neural value records"));
            }
            instructions = parsed_instructions.value();
            neural_values.reserve(static_cast<std::size_t>(neural_count.value()));
            for (std::uint64_t neural = 0U; neural < neural_count.value(); ++neural) {
                const auto value = read_string(input, "mutation changed neural value",
                                               kMaxTextBytes, true);
                if (!value) return core::Result<LineageGraph>::failure(value.error());
                neural_values.push_back(value.value());
            }
        }
        const auto outcome = read_string(input, "mutation outcome", kMaxTextBytes, true);
        const auto provenance = read_provenance(input);
        if (!id || !parent || !child || !parent_digest || !child_digest || !seed || !operation ||
            !genes || !outcome || !provenance) {
            return core::Result<LineageGraph>::failure(parse_error("invalid mutation receipt record"));
        }
        const auto inserted = graph.insert_mutation(MutationReceipt{
            MutationId{id.value()}, LineageId{parent.value()}, LineageId{child.value()},
            parent_digest.value(), child_digest.value(), seed.value(), operation.value(),
            genes.value(), std::move(instructions), std::move(neural_values), outcome.value(),
            provenance.value()});
        if (!inserted) return core::Result<LineageGraph>::failure(inserted.error());
    }
    if (version.value() >= 2U) {
        if (auto result = require_record(input, "RECOMBINATIONS"); !result) {
            return core::Result<LineageGraph>::failure(result.error());
        }
        const auto recombination_count = read_uint(input, "recombination receipt count");
        if (!recombination_count || recombination_count.value() > kMaxCollectionEntries) {
            return core::Result<LineageGraph>::failure(parse_error(
                "invalid recombination receipt count"));
        }
        for (std::uint64_t index = 0U; index < recombination_count.value(); ++index) {
            if (auto result = require_record(input, "RECOMBINATION"); !result) {
                return core::Result<LineageGraph>::failure(result.error());
            }
            const auto id = read_uint(input, "recombination receipt id");
            const auto parent_a = read_uint(input, "recombination first parent");
            const auto parent_b = read_uint(input, "recombination second parent");
            const auto child = read_uint(input, "recombination child lineage");
            const auto child_digest = read_optional_digest(input, "recombination child genome");
            const auto post_mutation = read_optional_id<MutationId>(
                input, "recombination post mutation");
            const auto module_count = read_uint(input, "recombination module origin count");
            if (!id || !parent_a || !parent_b || !child || !child_digest || !post_mutation ||
                !module_count || module_count.value() == 0U || module_count.value() > kMaxTags) {
                return core::Result<LineageGraph>::failure(parse_error(
                    "invalid recombination receipt header"));
            }
            std::vector<std::string> module_origins;
            module_origins.reserve(static_cast<std::size_t>(module_count.value()));
            for (std::uint64_t module = 0U; module < module_count.value(); ++module) {
                const auto origin = read_string(input, "recombination module origin",
                                                kMaxReferenceBytes, true);
                if (!origin) return core::Result<LineageGraph>::failure(origin.error());
                module_origins.push_back(origin.value());
            }
            const auto outcome = read_string(input, "recombination outcome", kMaxTextBytes, true);
            const auto provenance = read_provenance(input);
            if (!outcome || !provenance) {
                const auto& error = !outcome ? outcome.error() : provenance.error();
                return core::Result<LineageGraph>::failure(error);
            }
            const auto inserted = graph.insert_recombination(RecombinationReceipt{
                RecombinationId{id.value()}, LineageId{parent_a.value()}, LineageId{parent_b.value()},
                LineageId{child.value()}, child_digest.value(), std::move(module_origins),
                post_mutation.value(), outcome.value(), provenance.value()});
            if (!inserted) return core::Result<LineageGraph>::failure(inserted.error());
        }
    }
    if (auto result = require_record(input, "END"); !result) {
        return core::Result<LineageGraph>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<LineageGraph>::failure(
            parse_error("lineage graph record contains trailing data"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<LineageGraph>::failure(result.error());
    }
    return core::Result<LineageGraph>::success(std::move(graph));
}

core::Result<LineageGraphNewickExport> LineageGraph::export_newick() const {
    if (auto result = validate(); !result) {
        return core::Result<LineageGraphNewickExport>::failure(
            result.error().with_context("Newick lineage export"));
    }
    if (nodes_.empty()) {
        return core::Result<LineageGraphNewickExport>::failure(validation(
            "Newick export requires at least one lineage node"));
    }

    std::vector<LineageId> roots;
    std::map<LineageId, std::vector<LineageId>> children;
    const Provenance* envelope_provenance = nullptr;
    for (const auto& [id, node] : nodes_) {
        if (envelope_provenance == nullptr && !node.provenance.source_reference.empty()) {
            envelope_provenance = &node.provenance;
        }
        if (node.parent_b.has_value()) {
            return core::Result<LineageGraphNewickExport>::failure(validation(
                "Newick cannot represent reticulation or a second lineage parent"));
        }
        if (node.parent_a.has_value()) {
            children[*node.parent_a].push_back(id);
        } else {
            roots.push_back(id);
        }
    }
    if (roots.size() != 1U) {
        return core::Result<LineageGraphNewickExport>::failure(validation(
            "Newick export requires exactly one rooted connected tree"));
    }
    if (envelope_provenance == nullptr) {
        return core::Result<LineageGraphNewickExport>::failure(validation(
            "Newick export requires a node provenance source for its envelope"));
    }

    const auto label_for = [](LineageId id, const LineageNode& node) {
        return std::string("cartographer__") + std::to_string(id.value) + "__g" +
            std::to_string(node.generation) + "__" + encode_hex_text(node.source_identifier);
    };
    std::ostringstream output;
    output << "#cartographer.newick_version=1\n"
           << "#cartographer.tree_kind=rooted-single-parent\n"
           << "#cartographer.feature_identity=cartographer-lineage-v1\n"
           << "#cartographer.provenance_source_hex="
           << encode_hex_text(envelope_provenance->source_reference) << '\n'
           << "#cartographer.provenance_release_hex="
           << encode_hex_text(envelope_provenance->release) << '\n'
           << "#cartographer.provenance_license_hex="
           << encode_hex_text(envelope_provenance->license) << '\n'
           << "#cartographer.provenance_provider_hex="
           << encode_hex_text(envelope_provenance->provider) << '\n'
           << "#cartographer.provenance_imported_at_hex="
           << encode_hex_text(envelope_provenance->imported_at_utc) << '\n';

    const auto emit = [&](const auto& self, LineageId id) -> void {
        const auto child = children.find(id);
        if (child != children.end()) {
            output << '(';
            for (std::size_t index = 0U; index < child->second.size(); ++index) {
                if (index != 0U) output << ',';
                self(self, child->second[index]);
            }
            output << ')';
        }
        output << label_for(id, nodes_.at(id));
    };
    emit(emit, roots.front());
    output << ";\n";
    if (output.tellp() < 0 ||
        static_cast<std::size_t>(output.tellp()) > kMaxLineageNewickBytes) {
        return core::Result<LineageGraphNewickExport>::failure(validation(
            "Newick export exceeds the safety limit"));
    }

    LineageGraphNewickExport result;
    result.text = output.str();
    if (result.text.empty() || result.text.size() > kMaxLineageNewickBytes) {
        return core::Result<LineageGraphNewickExport>::failure(validation(
            "Newick export is empty or exceeds the safety limit"));
    }
    result.feature_loss_notes.push_back(
        "Newick preserves rooted single-parent topology, stable Cartographer lineage IDs, "
        "generation, and optional source identifiers in controlled labels");
    result.feature_loss_notes.push_back(
        "Newick omits per-node genome and morphology digests plus per-node provenance; "
        "import uses caller provenance");
    if (!mutations_.empty()) {
        result.feature_loss_notes.push_back(
            "Newick omits mutation receipts; they remain native Cartographer lineage evidence");
    }
    result.feature_loss_notes.push_back(
        "Newick is an exchange/reference tree only and does not imply phenotype, development, "
        "runtime behavior, or execution");
    return core::Result<LineageGraphNewickExport>::success(std::move(result));
}

core::Result<LineageGraph> LineageGraph::import_newick(
    std::string_view text,
    Provenance provenance) {
    if (text.size() > kMaxLineageNewickBytes) {
        return core::Result<LineageGraph>::failure(parse_error(
            "Newick record is too large"));
    }
    if (auto result = provenance.validate(); !result || provenance.source_reference.empty()) {
        return core::Result<LineageGraph>::failure(validation(
            "Newick import requires non-empty caller provenance"));
    }

    bool body_seen = false;
    std::string body;
    std::optional<std::string> version;
    std::optional<std::string> tree_kind;
    std::optional<std::string> feature_identity;
    std::optional<std::string> provenance_source_hex;
    std::optional<std::string> provenance_release_hex;
    std::optional<std::string> provenance_license_hex;
    std::optional<std::string> provenance_provider_hex;
    std::optional<std::string> provenance_imported_at_hex;
    const auto set_metadata = [](
        std::optional<std::string>& destination,
        std::string_view value,
        std::string_view field) -> core::Result<void> {
        if (destination.has_value()) {
            return core::Result<void>::failure(parse_error(
                std::string(field) + " is duplicated"));
        }
        destination = std::string(value);
        return core::Result<void>::success();
    };

    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!body_seen && line.starts_with("#")) {
            core::Result<void> parsed = core::Result<void>::failure(parse_error(
                "unsupported Newick metadata header"));
            if (line.starts_with("#cartographer.newick_version=")) {
                parsed = set_metadata(version, line.substr(
                    std::string_view("#cartographer.newick_version=").size()), "Newick version");
            } else if (line.starts_with("#cartographer.tree_kind=")) {
                parsed = set_metadata(tree_kind, line.substr(
                    std::string_view("#cartographer.tree_kind=").size()), "Newick tree kind");
            } else if (line.starts_with("#cartographer.feature_identity=")) {
                parsed = set_metadata(feature_identity, line.substr(
                    std::string_view("#cartographer.feature_identity=").size()),
                    "Newick feature identity");
            } else if (line.starts_with("#cartographer.provenance_source_hex=")) {
                parsed = set_metadata(provenance_source_hex, line.substr(
                    std::string_view("#cartographer.provenance_source_hex=").size()),
                                      "Newick provenance source");
            } else if (line.starts_with("#cartographer.provenance_release_hex=")) {
                parsed = set_metadata(provenance_release_hex, line.substr(
                    std::string_view("#cartographer.provenance_release_hex=").size()),
                                      "Newick provenance release");
            } else if (line.starts_with("#cartographer.provenance_license_hex=")) {
                parsed = set_metadata(provenance_license_hex, line.substr(
                    std::string_view("#cartographer.provenance_license_hex=").size()),
                                      "Newick provenance license");
            } else if (line.starts_with("#cartographer.provenance_provider_hex=")) {
                parsed = set_metadata(provenance_provider_hex, line.substr(
                    std::string_view("#cartographer.provenance_provider_hex=").size()),
                                      "Newick provenance provider");
            } else if (line.starts_with("#cartographer.provenance_imported_at_hex=")) {
                parsed = set_metadata(provenance_imported_at_hex, line.substr(
                    std::string_view("#cartographer.provenance_imported_at_hex=").size()),
                                      "Newick provenance imported-at");
            }
            if (!parsed) return core::Result<LineageGraph>::failure(parsed.error());
            continue;
        }
        if (line.starts_with("#")) {
            return core::Result<LineageGraph>::failure(parse_error(
                "Newick comments or metadata after the tree are unsupported"));
        }
        body_seen = true;
        body.append(line);
        if (body.size() > kMaxLineageNewickBytes) {
            return core::Result<LineageGraph>::failure(parse_error(
                "Newick tree exceeds the safety limit"));
        }
    }
    if (!body_seen || !version.has_value() || !tree_kind.has_value() ||
        !feature_identity.has_value() || !provenance_source_hex.has_value() ||
        !provenance_release_hex.has_value() || !provenance_license_hex.has_value() ||
        !provenance_provider_hex.has_value() || !provenance_imported_at_hex.has_value()) {
        return core::Result<LineageGraph>::failure(parse_error(
            "Newick is missing required Cartographer identity or provenance metadata"));
    }
    if (version.value() != "1" || tree_kind.value() != "rooted-single-parent" ||
        feature_identity.value() != "cartographer-lineage-v1") {
        return core::Result<LineageGraph>::failure(validation(
            "Newick identity metadata is unsupported"));
    }
    const auto envelope_source = decode_hex_text(
        provenance_source_hex.value(), "Newick provenance source", kMaxReferenceBytes, true);
    const auto envelope_release = decode_hex_text(
        provenance_release_hex.value(), "Newick provenance release", kMaxTextBytes, false);
    const auto envelope_license = decode_hex_text(
        provenance_license_hex.value(), "Newick provenance license", kMaxTextBytes, false);
    const auto envelope_provider = decode_hex_text(
        provenance_provider_hex.value(), "Newick provenance provider", kMaxTextBytes, false);
    const auto envelope_imported_at = decode_hex_text(
        provenance_imported_at_hex.value(), "Newick provenance imported-at", 128U, false);
    if (!envelope_source || !envelope_release || !envelope_license || !envelope_provider ||
        !envelope_imported_at) {
        return core::Result<LineageGraph>::failure(parse_error(
            "Newick provenance envelope is malformed"));
    }

    NewickParser parser(body);
    const auto parsed = parser.parse();
    if (!parsed) return core::Result<LineageGraph>::failure(parsed.error());

    struct ImportedIdentity {
        LineageId id;
        std::uint64_t generation = 0U;
        std::string source_identifier;
    };
    const auto identity_for = [](
        std::string_view label,
        std::string_view path,
        std::uint64_t depth) -> core::Result<ImportedIdentity> {
        if (label.starts_with("cartographer__")) {
            const std::string_view rest = label.substr(std::string_view("cartographer__").size());
            const auto generation_marker = rest.find("__g");
            if (generation_marker == std::string_view::npos) {
                return core::Result<ImportedIdentity>::failure(parse_error(
                    "Newick Cartographer label is missing its generation"));
            }
            const auto id = parse_newick_uint(rest.substr(0U, generation_marker),
                                              "Newick Cartographer lineage id");
            const auto generation_start = generation_marker + 3U;
            const auto source_marker = rest.find("__", generation_start);
            if (!id || source_marker == std::string_view::npos) {
                return core::Result<ImportedIdentity>::failure(parse_error(
                    "Newick Cartographer label is malformed"));
            }
            const auto generation = parse_newick_uint(
                rest.substr(generation_start, source_marker - generation_start),
                "Newick Cartographer generation");
            const auto source = decode_hex_text(
                rest.substr(source_marker + 2U), "Newick source identifier",
                kMaxReferenceBytes, false);
            if (!generation || !source || id.value() == 0U) {
                return core::Result<ImportedIdentity>::failure(parse_error(
                    "Newick Cartographer label contains invalid identity data"));
            }
            return core::Result<ImportedIdentity>::success({
                LineageId{id.value()}, generation.value(), source.value()});
        }
        if (label.empty()) {
            std::string key = "newick-path\0";
            key.append(path);
            return core::Result<ImportedIdentity>::success({
                LineageId{stable_lineage_newick_id(key)}, depth, {}});
        }
        std::string key = "newick-label\0";
        key.append(path);
        key.push_back('\0');
        key.append(label);
        return core::Result<ImportedIdentity>::success({
            LineageId{stable_lineage_newick_id(key)}, depth, std::string(label)});
    };

    std::vector<LineageNode> imported_nodes;
    std::set<LineageId> imported_ids;
    const auto collect = [&](const auto& self,
                             const NewickNodeSpec& spec,
                             std::optional<LineageId> parent,
                             std::uint64_t depth,
                             const std::string& path) -> core::Result<void> {
        if (depth > kMaxLineageNewickDepth || path.size() > kMaxReferenceBytes) {
            return core::Result<void>::failure(parse_error(
                "Newick tree exceeds the depth or path safety limit"));
        }
        const auto identity = identity_for(spec.label, path, depth);
        if (!identity) return core::Result<void>::failure(identity.error());
        if (!imported_ids.insert(identity.value().id).second) {
            return core::Result<void>::failure(validation(
                "Newick lineage identity is duplicated"));
        }
        imported_nodes.push_back(LineageNode{
            identity.value().id, parent, std::nullopt, identity.value().generation,
            std::nullopt, std::nullopt, provenance, identity.value().source_identifier});
        if (imported_nodes.size() > kMaxCollectionEntries) {
            return core::Result<void>::failure(parse_error(
                "Newick tree exceeds the node safety limit"));
        }
        for (std::size_t index = 0U; index < spec.children.size(); ++index) {
            const auto child_path = path + "." + std::to_string(index);
            if (auto result = self(self, spec.children[index], identity.value().id,
                                   depth + 1U, child_path); !result) {
                return result;
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = collect(collect, parsed.value(), std::nullopt, 0U, "r"); !result) {
        return core::Result<LineageGraph>::failure(result.error());
    }

    LineageGraph graph;
    for (auto& node : imported_nodes) {
        if (auto result = graph.insert_node(std::move(node)); !result) {
            return core::Result<LineageGraph>::failure(result.error());
        }
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<LineageGraph>::failure(result.error().with_context(
            "Newick lineage graph"));
    }
    return core::Result<LineageGraph>::success(std::move(graph));
}

core::Result<void> ScientificModel::insert_region(Region value) {
    return insert_unique(regions_, std::move(value), "region");
}

core::Result<void> ScientificModel::insert_boundary(Boundary value) {
    return insert_unique(boundaries_, std::move(value), "boundary");
}

core::Result<void> ScientificModel::insert_interface(Interface value) {
    return insert_unique(interfaces_, std::move(value), "interface");
}

core::Result<void> ScientificModel::insert_source(Source value) {
    return insert_unique(sources_, std::move(value), "source");
}

core::Result<void> ScientificModel::insert_load(Load value) {
    return insert_unique(loads_, std::move(value), "load");
}

core::Result<void> ScientificModel::insert_parameter(Parameter value) {
    return insert_unique(parameters_, std::move(value), "parameter");
}

core::Result<void> ScientificModel::insert_field(Field value) {
    return insert_unique(fields_, std::move(value), "field");
}

core::Result<void> ScientificModel::insert_probe(Probe value) {
    return insert_unique(probes_, std::move(value), "probe");
}

core::Result<void> ScientificModel::insert_study(Study value) {
    return insert_unique(studies_, std::move(value), "study");
}

core::Result<void> ScientificModel::insert_result_set(ResultSet value) {
    return insert_unique(result_sets_, std::move(value), "result set");
}

core::Result<ParameterSweepPreview> ScientificModel::preview_parameter_sweep(
    StudyId study,
    std::vector<ParameterSweepAxis> axes) const {
    if (auto result = validate(); !result) {
        return core::Result<ParameterSweepPreview>::failure(
            result.error().with_context("parameter sweep preview"));
    }
    const auto study_iterator = studies_.find(study);
    if (study_iterator == studies_.end()) {
        return core::Result<ParameterSweepPreview>::failure(invalid(
            "parameter sweep references an unknown study"));
    }
    if (axes.empty() || axes.size() > kMaxParameterSweepAxes) {
        return core::Result<ParameterSweepPreview>::failure(validation(
            "parameter sweep requires a bounded non-empty axis set"));
    }
    const auto& study_record = study_iterator->second;
    if (study_record.parameters.empty()) {
        return core::Result<ParameterSweepPreview>::failure(validation(
            "parameter sweep study has no parameters"));
    }

    std::vector<ParameterId> parameter_order = study_record.parameters;
    std::sort(parameter_order.begin(), parameter_order.end());
    if (std::adjacent_find(parameter_order.begin(), parameter_order.end()) !=
        parameter_order.end()) {
        return core::Result<ParameterSweepPreview>::failure(validation(
            "parameter sweep study contains a duplicate parameter"));
    }
    if (parameter_order.size() > kMaxParameterSweepParameters) {
        return core::Result<ParameterSweepPreview>::failure(validation(
            "parameter sweep study has too many parameters"));
    }

    ParameterSweepPreview preview;
    preview.study = study;
    preview.source_model_revision = study_record.source_model_revision;
    preview.strategy = "cartesian";
    preview.base_assignments.reserve(parameter_order.size());
    for (const auto parameter_id : parameter_order) {
        const auto& parameter = parameters_.at(parameter_id);
        preview.base_assignments.push_back(ParameterAssignment{
            parameter_id, parameter.value});
    }

    std::sort(axes.begin(), axes.end(), [](const ParameterSweepAxis& left,
                                           const ParameterSweepAxis& right) {
        return left.parameter < right.parameter;
    });
    const std::set<ParameterId> study_parameters(
        study_record.parameters.begin(), study_record.parameters.end());
    std::set<ParameterId> seen_axes;
    for (auto& axis : axes) {
        if (auto result = axis.validate(); !result) {
            return core::Result<ParameterSweepPreview>::failure(result.error());
        }
        if (!study_parameters.contains(axis.parameter)) {
            return core::Result<ParameterSweepPreview>::failure(validation(
                "parameter sweep axis references a parameter outside the study"));
        }
        if (!seen_axes.insert(axis.parameter).second) {
            return core::Result<ParameterSweepPreview>::failure(validation(
                "parameter sweep contains a duplicate axis"));
        }
        std::sort(axis.values.begin(), axis.values.end());
        if (std::adjacent_find(axis.values.begin(), axis.values.end()) != axis.values.end()) {
            return core::Result<ParameterSweepPreview>::failure(validation(
                "parameter sweep axis contains duplicate values"));
        }
        const auto& parameter = parameters_.at(axis.parameter);
        for (const double value : axis.values) {
            if (parameter.lower_bound.has_value() && value < *parameter.lower_bound) {
                return core::Result<ParameterSweepPreview>::failure(validation(
                    "parameter sweep value is below its authored bound"));
            }
            if (parameter.upper_bound.has_value() && value > *parameter.upper_bound) {
                return core::Result<ParameterSweepPreview>::failure(validation(
                    "parameter sweep value is above its authored bound"));
            }
        }
        preview.axes.push_back(std::move(axis));
    }

    preview.cases.push_back(preview.base_assignments);
    for (const auto& axis : preview.axes) {
        if (preview.cases.size() > kMaxParameterSweepCases / axis.values.size()) {
            return core::Result<ParameterSweepPreview>::failure(validation(
                "parameter sweep case count exceeds the safety limit"));
        }
        std::vector<std::vector<ParameterAssignment>> expanded;
        expanded.reserve(preview.cases.size() * axis.values.size());
        for (const auto& current : preview.cases) {
            for (const double value : axis.values) {
                auto next = current;
                const auto assignment = std::find_if(
                    next.begin(), next.end(), [&axis](const ParameterAssignment& candidate) {
                        return candidate.parameter == axis.parameter;
                    });
                if (assignment == next.end()) {
                    return core::Result<ParameterSweepPreview>::failure(validation(
                        "parameter sweep expansion lost an axis parameter"));
                }
                assignment->value = value;
                expanded.push_back(std::move(next));
            }
        }
        preview.cases = std::move(expanded);
    }
    if (auto result = preview.validate(); !result) {
        return core::Result<ParameterSweepPreview>::failure(result.error());
    }
    return core::Result<ParameterSweepPreview>::success(std::move(preview));
}

core::Result<void> ScientificModel::set_morphology(MorphologyModel value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("morphology admission"));
    }
    morphology_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_functional_genome(FunctionalGenome value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("functional genome admission"));
    }
    functional_genome_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_developmental_program(DevelopmentalProgram value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("developmental program admission"));
    }
    developmental_program_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_developmental_trace(DevelopmentalTrace value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("developmental trace admission"));
    }
    developmental_trace_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_lineage_graph(LineageGraph value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("lineage graph admission"));
    }
    lineage_graph_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_morphometrics(MorphometricModel value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("morphometric admission"));
    }
    morphometrics_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_sequence_genome(SequenceGenome value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("sequence genome admission"));
    }
    sequence_genome_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_anatomy(AnatomyModel value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("anatomy admission"));
    }
    anatomy_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_field_visualizations(FieldVisualizationModel value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("field visualization admission"));
    }
    field_visualizations_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_import_ledger(ScientificImportLedger value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context("scientific import ledger admission"));
    }
    import_ledger_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_phenotype_capabilities(PhenotypeCapabilityModel value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("phenotype capability admission"));
    }
    phenotype_capabilities_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_gene_regulatory_network(GeneRegulatoryNetwork value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("gene regulatory network admission"));
    }
    gene_regulatory_network_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_anatomy_provider_manifest(AnatomyProviderManifest value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("anatomy provider manifest admission"));
    }
    anatomy_provider_manifest_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::set_replay_capsules(ScientificReplayCapsuleModel value) {
    if (auto result = value.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("scientific replay capsule admission"));
    }
    replay_capsules_ = std::move(value);
    return core::Result<void>::success();
}

bool ScientificModel::empty() const noexcept {
    return regions_.empty() && boundaries_.empty() && interfaces_.empty() && sources_.empty() &&
        loads_.empty() && parameters_.empty() && fields_.empty() && studies_.empty() &&
        probes_.empty() && result_sets_.empty() && !morphology_.has_value() &&
        !functional_genome_.has_value() && !developmental_program_.has_value() &&
        !developmental_trace_.has_value() && !lineage_graph_.has_value() &&
        !morphometrics_.has_value() && !sequence_genome_.has_value() && !anatomy_.has_value() &&
        !field_visualizations_.has_value() && !import_ledger_.has_value() &&
        !phenotype_capabilities_.has_value() && !gene_regulatory_network_.has_value() &&
        !anatomy_provider_manifest_.has_value() && !replay_capsules_.has_value();
}

core::Result<void> DevelopmentalInstructionImpact::validate() const {
    if (auto result = validate_id(instruction.value, "developmental instruction impact instruction");
        !result) {
        return result;
    }
    if (auto result = validate_id(stage.value, "developmental instruction impact stage"); !result) {
        return result;
    }
    if (auto result = validate_id_list(planned_creates,
                                       "developmental instruction impact planned creates"); !result) {
        return result;
    }
    if (auto result = validate_id_list(planned_modifies,
                                       "developmental instruction impact planned modifies"); !result) {
        return result;
    }
    if (auto result = validate_id_list(trace_entries,
                                       "developmental instruction impact trace entries"); !result) {
        return result;
    }
    if (auto result = validate_id_list(observed_creates,
                                       "developmental instruction impact observed creates"); !result) {
        return result;
    }
    if (auto result = validate_id_list(observed_modifies,
                                       "developmental instruction impact observed modifies"); !result) {
        return result;
    }
    return core::Result<void>::success();
}

core::Result<std::vector<DevelopmentalTimelineStep>>
ScientificModel::build_developmental_timeline() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<DevelopmentalTimelineStep>>::failure(
            result.error().with_context("developmental timeline projection"));
    }
    if (!developmental_program_.has_value()) {
        return core::Result<std::vector<DevelopmentalTimelineStep>>::success({});
    }

    std::vector<DevelopmentalTimelineStep> timeline;
    timeline.reserve(developmental_program_->stages().size());
    for (const auto& [stage_id, stage] : developmental_program_->stages()) {
        std::set<InstructionId> instruction_ids;
        std::set<TraceId> trace_ids;
        std::set<StructureId> planned_creates;
        std::set<StructureId> planned_modifies;
        std::set<StructureId> observed_creates;
        std::set<StructureId> observed_modifies;

        for (const auto& [instruction_id, instruction] : developmental_program_->instructions()) {
            if (instruction.stage != stage_id) continue;
            instruction_ids.insert(instruction_id);
            planned_creates.insert(instruction.creates.begin(), instruction.creates.end());
            planned_modifies.insert(instruction.modifies.begin(), instruction.modifies.end());
        }
        if (developmental_trace_.has_value()) {
            for (const auto& [trace_id, entry] : developmental_trace_->entries()) {
                if (entry.stage != stage_id) continue;
                trace_ids.insert(trace_id);
                observed_creates.insert(entry.created.begin(), entry.created.end());
                observed_modifies.insert(entry.modified.begin(), entry.modified.end());
            }
        }

        DevelopmentalTimelineStep step;
        step.stage = stage_id;
        step.ordinal = stage.ordinal;
        step.stage_name = stage.name;
        step.instructions.assign(instruction_ids.begin(), instruction_ids.end());
        step.trace_entries.assign(trace_ids.begin(), trace_ids.end());
        step.planned_creates.assign(planned_creates.begin(), planned_creates.end());
        step.planned_modifies.assign(planned_modifies.begin(), planned_modifies.end());
        step.observed_creates.assign(observed_creates.begin(), observed_creates.end());
        step.observed_modifies.assign(observed_modifies.begin(), observed_modifies.end());
        timeline.push_back(std::move(step));
    }
    std::stable_sort(timeline.begin(), timeline.end(), [](const auto& left, const auto& right) {
        if (left.ordinal != right.ordinal) return left.ordinal < right.ordinal;
        return left.stage < right.stage;
    });
    return core::Result<std::vector<DevelopmentalTimelineStep>>::success(std::move(timeline));
}

core::Result<DevelopmentalInstructionImpact>
ScientificModel::build_developmental_instruction_impact(InstructionId instruction_id) const {
    if (auto result = validate(); !result) {
        return core::Result<DevelopmentalInstructionImpact>::failure(
            result.error().with_context("developmental instruction impact projection"));
    }
    if (auto result = validate_id(instruction_id.value,
                                  "developmental instruction impact instruction"); !result) {
        return core::Result<DevelopmentalInstructionImpact>::failure(result.error());
    }
    if (!developmental_program_.has_value()) {
        return core::Result<DevelopmentalInstructionImpact>::failure(validation(
            "developmental instruction impact requires an admitted developmental program"));
    }
    const auto instruction_iterator = developmental_program_->instructions().find(instruction_id);
    if (instruction_iterator == developmental_program_->instructions().end()) {
        return core::Result<DevelopmentalInstructionImpact>::failure(validation(
            "developmental instruction impact references an unknown instruction"));
    }

    DevelopmentalInstructionImpact impact;
    impact.instruction = instruction_id;
    impact.stage = instruction_iterator->second.stage;
    impact.planned_creates = instruction_iterator->second.creates;
    impact.planned_modifies = instruction_iterator->second.modifies;
    std::set<TraceId> trace_ids;
    std::set<StructureId> observed_creates;
    std::set<StructureId> observed_modifies;
    if (developmental_trace_.has_value()) {
        for (const auto& [trace_id, entry] : developmental_trace_->entries()) {
            if (entry.instruction != instruction_id) continue;
            trace_ids.insert(trace_id);
            observed_creates.insert(entry.created.begin(), entry.created.end());
            observed_modifies.insert(entry.modified.begin(), entry.modified.end());
        }
    }
    impact.trace_entries.assign(trace_ids.begin(), trace_ids.end());
    impact.observed_creates.assign(observed_creates.begin(), observed_creates.end());
    impact.observed_modifies.assign(observed_modifies.begin(), observed_modifies.end());
    if (auto result = impact.validate(); !result) {
        return core::Result<DevelopmentalInstructionImpact>::failure(result.error());
    }
    return core::Result<DevelopmentalInstructionImpact>::success(std::move(impact));
}

core::Result<EvolutionaryMorphospacePreview>
ScientificModel::preview_evolutionary_morphospace(
    std::span<const EvolutionaryMorphospaceBinding> bindings,
    std::uint32_t dimensions,
    std::string_view normalization) const {
    if (!morphometrics_.has_value() || !lineage_graph_.has_value()) {
        return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
            "evolutionary morphospace requires admitted morphometrics and lineage graph"));
    }
    if (auto result = validate(); !result) {
        return core::Result<EvolutionaryMorphospacePreview>::failure(
            result.error().with_context("evolutionary morphospace preview"));
    }
    if (bindings.empty() || bindings.size() > kMaxMorphospacePoints) {
        return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
            "evolutionary morphospace requires a bounded non-empty binding set"));
    }
    if (dimensions != 2U && dimensions != 3U) {
        return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
            "evolutionary morphospace dimensions must be 2 or 3"));
    }
    if (normalization != "raw" && normalization != "unit-box") {
        return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
            "evolutionary morphospace normalization is unsupported"));
    }

    std::vector<EvolutionaryMorphospaceBinding> ordered(bindings.begin(), bindings.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.lineage != right.lineage) return left.lineage < right.lineage;
        return left.signature < right.signature;
    });

    EvolutionaryMorphospacePreview preview;
    preview.projection = "leading-components";
    preview.normalization = std::string(normalization);
    preview.dimensions = dimensions;

    std::set<LineageId> seen_lineages;
    std::set<SignatureId> seen_signatures;
    std::array<double, 3> lower{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
    };
    std::array<double, 3> upper{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    std::vector<std::array<double, 3>> raw_positions;
    raw_positions.reserve(ordered.size());

    for (const auto& binding : ordered) {
        if (auto result = binding.validate(); !result) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(result.error());
        }
        if (!seen_lineages.insert(binding.lineage).second) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                "evolutionary morphospace binds more than one signature to a lineage"));
        }
        if (!seen_signatures.insert(binding.signature).second) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                "evolutionary morphospace reuses a signature for multiple lineages"));
        }
        const auto lineage_iterator = lineage_graph_->nodes().find(binding.lineage);
        if (lineage_iterator == lineage_graph_->nodes().end()) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                "evolutionary morphospace references an unknown lineage"));
        }
        const auto signature_iterator = morphometrics_->signatures().find(binding.signature);
        if (signature_iterator == morphometrics_->signatures().end()) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                "evolutionary morphospace references an unknown signature"));
        }
        const auto& signature = signature_iterator->second;
        if (signature.components.size() < dimensions) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                "evolutionary morphospace signature has too few components"));
        }
        if (preview.points.empty()) {
            preview.descriptor = signature.descriptor;
            preview.coordinate_frame = signature.coordinate_frame;
            preview.signature_normalization = signature.normalization;
            if (!signature.component_labels.empty()) {
                preview.component_labels.assign(
                    signature.component_labels.begin(),
                    signature.component_labels.begin() + static_cast<std::ptrdiff_t>(dimensions));
            }
        } else if (signature.descriptor != preview.descriptor ||
                   signature.coordinate_frame != preview.coordinate_frame ||
                   signature.normalization != preview.signature_normalization ||
                   (signature.component_labels.empty() != preview.component_labels.empty()) ||
                   (!signature.component_labels.empty() &&
                    !std::equal(preview.component_labels.begin(), preview.component_labels.end(),
                                signature.component_labels.begin()))) {
            return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                "evolutionary morphospace requires matching signature descriptor, frame, and labels"));
        }

        std::array<double, 3> raw{
            signature.components[0U],
            signature.components[1U],
            dimensions == 3U ? signature.components[2U] : 0.0,
        };
        for (std::size_t axis = 0U; axis < dimensions; ++axis) {
            if (!std::isfinite(raw[axis])) {
                return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                    "evolutionary morphospace component is not finite"));
            }
            lower[axis] = std::min(lower[axis], raw[axis]);
            upper[axis] = std::max(upper[axis], raw[axis]);
        }
        raw_positions.push_back(raw);
        preview.points.push_back(EvolutionaryMorphospacePoint{
            binding.lineage,
            binding.signature,
            lineage_iterator->second.generation,
            core::Vec3d{raw[0U], raw[1U], raw[2U]},
        });
    }

    if (normalization == "unit-box") {
        for (std::size_t point_index = 0U; point_index < preview.points.size(); ++point_index) {
            auto& point = preview.points[point_index];
            auto& raw = raw_positions[point_index];
            std::array<double, 3> normalized{0.0, 0.0, 0.0};
            for (std::size_t axis = 0U; axis < dimensions; ++axis) {
                const double span = upper[axis] - lower[axis];
                if (!std::isfinite(span)) {
                    return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                        "evolutionary morphospace component range is not finite"));
                }
                normalized[axis] = span == 0.0 ? 0.0 : (raw[axis] - lower[axis]) / span;
                if (!std::isfinite(normalized[axis])) {
                    return core::Result<EvolutionaryMorphospacePreview>::failure(validation(
                        "evolutionary morphospace normalization is not finite"));
                }
            }
            point.position = core::Vec3d{normalized[0U], normalized[1U], normalized[2U]};
        }
    }

    std::map<LineageId, std::size_t> point_indices;
    for (std::size_t index = 0U; index < preview.points.size(); ++index) {
        point_indices.emplace(preview.points[index].lineage, index);
    }
    const auto append_parent_edge = [&preview, &point_indices](
                                        LineageId parent,
                                        LineageId child) {
        const auto parent_iterator = point_indices.find(parent);
        const auto child_iterator = point_indices.find(child);
        if (parent_iterator == point_indices.end() || child_iterator == point_indices.end()) {
            return;
        }
        const auto& parent_position = preview.points[parent_iterator->second].position;
        const auto& child_position = preview.points[child_iterator->second].position;
        const auto displacement = child_position - parent_position;
        const double distance = std::hypot(
            std::hypot(displacement.x, displacement.y), displacement.z);
        preview.trajectories.push_back(EvolutionaryMorphospaceEdge{
            parent, child, displacement, distance});
    };
    for (const auto& point : preview.points) {
        const auto& node = lineage_graph_->nodes().at(point.lineage);
        if (node.parent_a.has_value()) append_parent_edge(*node.parent_a, point.lineage);
        if (node.parent_b.has_value()) append_parent_edge(*node.parent_b, point.lineage);
    }
    std::sort(preview.trajectories.begin(), preview.trajectories.end(),
              [](const auto& left, const auto& right) {
                  if (left.child != right.child) return left.child < right.child;
                  return left.parent < right.parent;
              });

    if (auto result = preview.validate(); !result) {
        return core::Result<EvolutionaryMorphospacePreview>::failure(result.error());
    }
    return core::Result<EvolutionaryMorphospacePreview>::success(std::move(preview));
}

core::Result<void> ScientificModel::validate() const {
    if (regions_.size() > kMaxCollectionEntries || boundaries_.size() > kMaxCollectionEntries ||
        interfaces_.size() > kMaxCollectionEntries || sources_.size() > kMaxCollectionEntries ||
        loads_.size() > kMaxCollectionEntries || parameters_.size() > kMaxCollectionEntries ||
        fields_.size() > kMaxCollectionEntries || probes_.size() > kMaxCollectionEntries ||
        studies_.size() > kMaxCollectionEntries ||
        result_sets_.size() > kMaxCollectionEntries) {
        return core::Result<void>::failure(validation("scientific model collection exceeds the safety limit"));
    }
    for (const auto& [id, region] : regions_) {
        static_cast<void>(id);
        if (auto result = region.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific region"));
        }
        if (region.parent_region.has_value() && !regions_.contains(*region.parent_region)) {
            return core::Result<void>::failure(validation("region parent references an unknown region"));
        }
    }
    for (const auto& [id, boundary] : boundaries_) {
        static_cast<void>(id);
        if (auto result = boundary.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific boundary"));
        }
        if (auto result = validate_optional_reference(boundary.region, "boundary region", regions_); !result) return result;
    }
    for (const auto& [id, relation] : interfaces_) {
        static_cast<void>(id);
        if (auto result = relation.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific interface"));
        }
        if (auto result = validate_reference(regions_, relation.first_region, "interface first region"); !result) return result;
        if (auto result = validate_reference(regions_, relation.second_region, "interface second region"); !result) return result;
    }
    for (const auto& [id, source] : sources_) {
        static_cast<void>(id);
        if (auto result = source.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific source"));
        }
        if (auto result = validate_optional_reference(source.region, "source region", regions_); !result) return result;
    }
    for (const auto& [id, load] : loads_) {
        static_cast<void>(id);
        if (auto result = load.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific load"));
        }
        if (auto result = validate_optional_reference(load.region, "load region", regions_); !result) return result;
    }
    for (const auto& [id, parameter] : parameters_) {
        static_cast<void>(id);
        if (auto result = parameter.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific parameter"));
        }
    }
    for (const auto& [id, field] : fields_) {
        static_cast<void>(id);
        if (auto result = field.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific field"));
        }
        if (auto result = validate_optional_reference(field.source_study, "field source study", studies_); !result) return result;
        if (auto result = validate_optional_reference(field.sample_domain, "field sample domain", regions_); !result) return result;
    }
    std::map<RegionId, std::uint8_t> visit;
    const auto visit_region = [&](const auto& self, RegionId id) -> core::Result<void> {
        const auto state = visit[id];
        if (state == 1U) return core::Result<void>::failure(validation("region hierarchy contains a cycle"));
        if (state == 2U) return core::Result<void>::success();
        visit[id] = 1U;
        const auto& region = regions_.at(id);
        if (region.parent_region.has_value()) {
            if (auto result = self(self, *region.parent_region); !result) return result;
        }
        visit[id] = 2U;
        return core::Result<void>::success();
    };
    for (const auto& [id, region] : regions_) {
        static_cast<void>(region);
        if (auto result = visit_region(visit_region, id); !result) return result;
    }
    for (const auto& [id, study] : studies_) {
        static_cast<void>(id);
        if (auto result = study.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific study"));
        }
        if (auto result = validate_reference_list(regions_, study.regions, "study regions"); !result) return result;
        if (auto result = validate_reference_list(boundaries_, study.boundaries, "study boundaries"); !result) return result;
        if (auto result = validate_reference_list(interfaces_, study.interfaces, "study interfaces"); !result) return result;
        if (auto result = validate_reference_list(sources_, study.sources, "study sources"); !result) return result;
        if (auto result = validate_reference_list(loads_, study.loads, "study loads"); !result) return result;
        if (auto result = validate_reference_list(parameters_, study.parameters, "study parameters"); !result) return result;
        if (auto result = validate_reference_list(fields_, study.requested_outputs, "study requested outputs"); !result) return result;
    }
    for (const auto& [id, probe] : probes_) {
        static_cast<void>(id);
        if (auto result = probe.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific probe"));
        }
        if (auto result = validate_optional_reference(probe.region, "probe region", regions_); !result) return result;
        if (auto result = validate_optional_reference(probe.field, "probe field", fields_); !result) return result;
    }
    for (const auto& [id, result_set] : result_sets_) {
        static_cast<void>(id);
        if (auto result = result_set.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific result set"));
        }
        if (auto result = validate_reference(studies_, result_set.source_study, "result set source study"); !result) return result;
        if (auto result = validate_reference_list(fields_, result_set.fields, "result set fields"); !result) return result;
        if (auto result = validate_reference_list(probes_, result_set.probes, "result set probes"); !result) return result;
    }
    if (morphology_.has_value()) {
        if (auto result = morphology_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific morphology"));
        }
        for (const auto& [id, growth_domain] : morphology_->growth_domains()) {
            static_cast<void>(id);
            if (auto result = validate_optional_reference(
                    growth_domain.driving_field, "morphology growth domain driving field", fields_);
                !result) {
                return result;
            }
        }
    }
    if (functional_genome_.has_value()) {
        if (auto result = functional_genome_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific functional genome"));
        }
    }
    if (developmental_program_.has_value()) {
        if (auto result = developmental_program_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific developmental program"));
        }
        for (const auto& [id, instruction] : developmental_program_->instructions()) {
            static_cast<void>(id);
            if (auto result = validate_reference_list(parameters_, instruction.parameters,
                                                      "development instruction parameters"); !result) {
                return result;
            }
            if (morphology_.has_value()) {
                if (auto result = validate_reference_list(morphology_->structures(), instruction.creates,
                                                          "development instruction created structures"); !result) {
                    return result;
                }
                if (auto result = validate_reference_list(morphology_->structures(), instruction.modifies,
                                                          "development instruction modified structures"); !result) {
                    return result;
                }
            } else if (!instruction.creates.empty() || !instruction.modifies.empty()) {
                return core::Result<void>::failure(validation(
                    "development instruction targets morphology without a morphology model"));
            }
        }
        if (functional_genome_.has_value() &&
            !functional_genome_->developmental_program_reference.empty() &&
            functional_genome_->developmental_program_reference != developmental_program_->reference) {
            return core::Result<void>::failure(validation(
                "functional genome references a different developmental program"));
        }
    }
    if (developmental_trace_.has_value()) {
        if (auto result = developmental_trace_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific developmental trace"));
        }
        if (!developmental_program_.has_value()) {
            return core::Result<void>::failure(validation(
                "developmental trace requires an admitted developmental program"));
        }
        if (developmental_trace_->program_reference != developmental_program_->reference) {
            return core::Result<void>::failure(validation(
                "developmental trace references a different developmental program"));
        }
        if (developmental_trace_->program_digest.has_value() &&
            (!developmental_program_->source_digest.has_value() ||
             *developmental_trace_->program_digest != *developmental_program_->source_digest)) {
            return core::Result<void>::failure(validation(
                "developmental trace program digest does not match the admitted program source"));
        }
        if (functional_genome_.has_value() &&
            !developmental_trace_->genome_reference.empty() &&
            developmental_trace_->genome_reference != functional_genome_->developmental_program_reference) {
            return core::Result<void>::failure(validation(
                "developmental trace genome reference does not match the admitted genome program reference"));
        }
        if (developmental_trace_->genome_digest.has_value() &&
            (!functional_genome_.has_value() ||
             !functional_genome_->source_digest.has_value() ||
             *developmental_trace_->genome_digest != *functional_genome_->source_digest)) {
            return core::Result<void>::failure(validation(
                "developmental trace genome digest does not match the admitted functional genome source"));
        }
        for (const auto& [id, entry] : developmental_trace_->entries()) {
            static_cast<void>(id);
            if (auto result = validate_reference(developmental_program_->stages(), entry.stage,
                                                 "development trace stage"); !result) return result;
            if (auto result = validate_reference(developmental_program_->instructions(), entry.instruction,
                                                 "development trace instruction"); !result) return result;
            const auto& instruction = developmental_program_->instructions().at(entry.instruction);
            if (instruction.stage != entry.stage) {
                return core::Result<void>::failure(validation(
                    "development trace entry stage does not match its instruction stage"));
            }
            if (!entry.parameter_values.empty()) {
                if (entry.parameter_values.size() != instruction.parameters.size()) {
                    return core::Result<void>::failure(validation(
                        "development trace parameter values do not match instruction parameters"));
                }
                for (std::size_t index = 0U; index < entry.parameter_values.size(); ++index) {
                    const auto& parameter = parameters_.at(instruction.parameters[index]);
                    const auto value = entry.parameter_values[index];
                    if (parameter.lower_bound.has_value() && value < *parameter.lower_bound) {
                        return core::Result<void>::failure(validation(
                            "development trace parameter value is below its authored bound"));
                    }
                    if (parameter.upper_bound.has_value() && value > *parameter.upper_bound) {
                        return core::Result<void>::failure(validation(
                            "development trace parameter value is above its authored bound"));
                    }
                }
            }
            if (entry.tissue_region.has_value()) {
                if (!anatomy_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "development trace tissue assignment requires an admitted anatomy model"));
                }
                if (auto result = validate_reference(
                        anatomy_->tissue_regions(), *entry.tissue_region,
                        "development trace tissue region"); !result) {
                    return result;
                }
            }
            if (morphology_.has_value()) {
                if (auto result = validate_reference_list(morphology_->structures(), entry.created,
                                                          "development trace created structures"); !result) {
                    return result;
                }
                if (auto result = validate_reference_list(morphology_->structures(), entry.modified,
                                                          "development trace modified structures"); !result) {
                    return result;
                }
            } else if (!entry.created.empty() || !entry.modified.empty()) {
                return core::Result<void>::failure(validation(
                    "development trace targets morphology without a morphology model"));
            }
        }
    }
    if (lineage_graph_.has_value()) {
        if (auto result = lineage_graph_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific lineage graph"));
        }
        if (functional_genome_.has_value()) {
            for (const auto& [id, mutation] : lineage_graph_->mutations()) {
                static_cast<void>(id);
                if (auto result = validate_reference_list(functional_genome_->genes(),
                                                          mutation.changed_genes,
                                                          "mutation changed genes"); !result) {
                    return result;
                }
            }
        } else {
            for (const auto& [id, mutation] : lineage_graph_->mutations()) {
                static_cast<void>(id);
                if (!mutation.changed_genes.empty()) {
                    return core::Result<void>::failure(validation(
                        "mutation receipt names genes without an admitted functional genome"));
                }
            }
        }
        if (developmental_program_.has_value()) {
            for (const auto& [id, mutation] : lineage_graph_->mutations()) {
                static_cast<void>(id);
                if (auto result = validate_reference_list(
                        developmental_program_->instructions(), mutation.changed_instructions,
                        "mutation changed instructions"); !result) {
                    return result;
                }
            }
        } else {
            for (const auto& [id, mutation] : lineage_graph_->mutations()) {
                static_cast<void>(id);
                if (!mutation.changed_instructions.empty()) {
                    return core::Result<void>::failure(validation(
                        "mutation receipt names instructions without an admitted developmental program"));
                }
            }
        }
    }
    if (morphometrics_.has_value()) {
        if (auto result = morphometrics_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific morphometrics"));
        }
        if (!morphology_.has_value()) {
            return core::Result<void>::failure(validation(
                "morphometrics require an admitted morphology model"));
        }
        for (const auto& [id, observation] : morphometrics_->observations()) {
            static_cast<void>(id);
            if (auto result = validate_reference(morphology_->structures(), observation.structure,
                                                 "morphometric observation structure"); !result) return result;
        }
        for (const auto& [id, signature] : morphometrics_->signatures()) {
            static_cast<void>(id);
            if (auto result = validate_reference(morphology_->structures(), signature.structure,
                                                 "morphology signature structure"); !result) return result;
        }
        for (const auto& [id, comparison] : morphometrics_->comparisons()) {
            static_cast<void>(id);
            if (auto result = validate_reference(morphology_->structures(), comparison.reference_structure,
                                                 "morphology comparison reference structure"); !result) return result;
            if (auto result = validate_reference(morphology_->structures(), comparison.candidate_structure,
                                                 "morphology comparison candidate structure"); !result) return result;
            if (auto result = validate_optional_reference(comparison.reference_signature,
                                                          "morphology comparison reference signature",
                                                          morphometrics_->signatures()); !result) return result;
            if (auto result = validate_optional_reference(comparison.candidate_signature,
                                                          "morphology comparison candidate signature",
                                                          morphometrics_->signatures()); !result) return result;
            if (comparison.reference_signature.has_value()) {
                const auto& signature = morphometrics_->signatures().at(*comparison.reference_signature);
                if (signature.structure != comparison.reference_structure) {
                    return core::Result<void>::failure(validation(
                        "morphology comparison reference signature targets the wrong structure"));
                }
            }
            if (comparison.candidate_signature.has_value()) {
                const auto& signature = morphometrics_->signatures().at(*comparison.candidate_signature);
                if (signature.structure != comparison.candidate_structure) {
                    return core::Result<void>::failure(validation(
                        "morphology comparison candidate signature targets the wrong structure"));
                }
            }
        }
    }
    if (sequence_genome_.has_value()) {
        if (auto result = sequence_genome_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific sequence genome"));
        }
    }
    if (anatomy_.has_value()) {
        if (auto result = anatomy_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific anatomy"));
        }
        if (!anatomy_->parts().empty() && !morphology_.has_value()) {
            return core::Result<void>::failure(validation(
                "anatomy parts require an admitted morphology model"));
        }
        if (morphology_.has_value()) {
            for (const auto& [id, part] : anatomy_->parts()) {
                static_cast<void>(id);
                if (auto result = validate_reference(morphology_->structures(), part.structure,
                                                     "anatomy part structure"); !result) return result;
            }
        }
        for (const auto& [id, tissue_region] : anatomy_->tissue_regions()) {
            static_cast<void>(id);
            if (tissue_region.structure.has_value()) {
                if (!morphology_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "tissue region names a structure without an admitted morphology model"));
                }
                if (auto result = validate_reference(
                        morphology_->structures(), *tissue_region.structure,
                        "tissue region structure"); !result) {
                    return result;
                }
            }
            if (auto result = validate_optional_reference(
                    tissue_region.region, "tissue region region", regions_); !result) {
                return result;
            }
        }
    }
    if (field_visualizations_.has_value()) {
        if (auto result = field_visualizations_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific field visualizations"));
        }
        for (const auto& [id, visualization] : field_visualizations_->visualizations()) {
            static_cast<void>(id);
            if (auto result = validate_reference(fields_, visualization.field,
                                                 "field visualization field"); !result) return result;
            if (auto result = validate_optional_reference(visualization.domain,
                                                          "field visualization domain", regions_); !result) {
                return result;
            }
        }
    }
    if (import_ledger_.has_value()) {
        if (auto result = import_ledger_->validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("scientific import ledger"));
        }
    }
    if (phenotype_capabilities_.has_value()) {
        if (auto result = phenotype_capabilities_->validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("scientific phenotype capabilities"));
        }
        for (const auto& [id, capability] : phenotype_capabilities_->capabilities()) {
            static_cast<void>(id);
            if (!capability.contributing_structures.empty()) {
                if (!morphology_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "phenotype capability names structures without an admitted morphology model"));
                }
                if (auto result = validate_reference_list(
                        morphology_->structures(), capability.contributing_structures,
                        "phenotype capability contributing structures"); !result) {
                    return result;
                }
            }
            if (!capability.contributing_genes.empty()) {
                if (!functional_genome_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "phenotype capability names genes without an admitted functional genome"));
                }
                if (auto result = validate_reference_list(
                        functional_genome_->genes(), capability.contributing_genes,
                        "phenotype capability contributing genes"); !result) {
                    return result;
                }
            }
            if (!capability.contributing_instructions.empty()) {
                if (!developmental_program_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "phenotype capability names instructions without an admitted developmental program"));
                }
                if (auto result = validate_reference_list(
                        developmental_program_->instructions(), capability.contributing_instructions,
                        "phenotype capability contributing instructions"); !result) {
                    return result;
                }
            }
            if (!capability.contributing_traces.empty()) {
                if (!developmental_trace_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "phenotype capability names traces without an admitted developmental trace"));
                }
                if (auto result = validate_reference_list(
                        developmental_trace_->entries(), capability.contributing_traces,
                        "phenotype capability contributing traces"); !result) {
                    return result;
                }
            }
        }
    }
    if (gene_regulatory_network_.has_value()) {
        if (auto result = gene_regulatory_network_->validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("scientific gene regulatory network"));
        }
        const auto has_network_records = !gene_regulatory_network_->elements().empty() ||
            !gene_regulatory_network_->edges().empty() ||
            !gene_regulatory_network_->expressions().empty();
        if (has_network_records && !functional_genome_.has_value()) {
            return core::Result<void>::failure(validation(
                "gene regulatory network records require an admitted functional genome"));
        }
        for (const auto& [id, element] : gene_regulatory_network_->elements()) {
            static_cast<void>(id);
            if (auto result = validate_reference(functional_genome_->genes(), element.gene,
                                                 "regulatory element gene"); !result) {
                return result;
            }
        }
        for (const auto& [id, edge] : gene_regulatory_network_->edges()) {
            static_cast<void>(id);
            if (auto result = validate_reference(functional_genome_->genes(), edge.source_gene,
                                                 "regulatory edge source gene"); !result) {
                return result;
            }
            if (auto result = validate_reference(functional_genome_->genes(), edge.target_gene,
                                                 "regulatory edge target gene"); !result) {
                return result;
            }
            if (edge.stage.has_value()) {
                if (!developmental_program_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "regulatory edge stage requires an admitted developmental program"));
                }
                if (auto result = validate_reference(developmental_program_->stages(), *edge.stage,
                                                     "regulatory edge stage"); !result) return result;
            }
        }
        for (const auto& [id, expression] : gene_regulatory_network_->expressions()) {
            static_cast<void>(id);
            if (auto result = validate_reference(functional_genome_->genes(), expression.gene,
                                                 "spatial expression gene"); !result) {
                return result;
            }
            if (expression.structure.has_value()) {
                if (!morphology_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "spatial expression structure requires an admitted morphology model"));
                }
                if (auto result = validate_reference(morphology_->structures(), *expression.structure,
                                                     "spatial expression structure"); !result) return result;
            }
            if (auto result = validate_optional_reference(expression.region,
                                                          "spatial expression region", regions_);
                !result) return result;
            if (auto result = validate_optional_reference(expression.field,
                                                          "spatial expression field", fields_);
                !result) return result;
            if (expression.stage.has_value()) {
                if (!developmental_program_.has_value()) {
                    return core::Result<void>::failure(validation(
                        "spatial expression stage requires an admitted developmental program"));
                }
                if (auto result = validate_reference(developmental_program_->stages(), *expression.stage,
                                                     "spatial expression stage"); !result) return result;
            }
        }
    }
    if (anatomy_provider_manifest_.has_value()) {
        if (auto result = anatomy_provider_manifest_->validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("scientific anatomy provider manifest"));
        }
        if (!anatomy_provider_manifest_->bindings().empty() && !anatomy_.has_value()) {
            return core::Result<void>::failure(validation(
                "anatomy provider bindings require an admitted anatomy model"));
        }
        for (const auto& [id, binding] : anatomy_provider_manifest_->bindings()) {
            static_cast<void>(id);
            if (auto result = validate_reference(anatomy_->parts(), binding.part,
                                                 "anatomy provider binding part"); !result) {
                return result;
            }
            const auto& part = anatomy_->parts().at(binding.part);
            if (!morphology_.has_value()) {
                return core::Result<void>::failure(validation(
                    "anatomy provider bindings require an admitted morphology model"));
            }
            if (auto result = validate_reference(morphology_->structures(), part.structure,
                                                 "anatomy provider binding structure"); !result) {
                return result;
            }
        }
    }
    if (replay_capsules_.has_value()) {
        if (auto result = replay_capsules_->validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("scientific replay capsules"));
        }
        for (const auto& [id, capsule] : replay_capsules_->capsules()) {
            static_cast<void>(id);
            if (auto result = validate_reference_list(
                    parameters_, capsule.parameters, "scientific replay parameters"); !result) {
                return result;
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ScientificModel::validate_anatomy_provider_candidate(
    core::Revision expected_source_revision) const {
    if (auto result = validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "anatomy provider candidate"));
    }
    if (!anatomy_provider_manifest_.has_value()) {
        return core::Result<void>::failure(validation(
            "anatomy provider candidate requires an admitted provider manifest"));
    }
    if (auto result = anatomy_provider_manifest_->validate_handoff_candidate(
            expected_source_revision); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "anatomy provider candidate"));
    }
    return core::Result<void>::success();
}

core::Result<AnatomyProviderCandidate> ScientificModel::build_anatomy_provider_candidate(
    core::Revision expected_source_revision) const {
    if (auto result = validate_anatomy_provider_candidate(expected_source_revision); !result) {
        return core::Result<AnatomyProviderCandidate>::failure(result.error());
    }
    return AnatomyProviderCandidate::from_manifest(
        *anatomy_provider_manifest_, expected_source_revision);
}

std::string ScientificModel::serialize() const {
    std::ostringstream output;
    output << "CARTOGRAPHER_SCIENTIFIC_MODEL " << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "REGIONS " << regions_.size() << '\n';
    for (const auto& [id, region] : regions_) {
        output << "REGION " << id.value << ' ' << optional_id(region.parent_region) << ' '
               << std::quoted(region.semantic_type) << ' ' << std::quoted(region.geometry.kind) << ' '
               << std::quoted(region.geometry.reference) << ' '
               << std::quoted(region.material_or_tissue_binding) << ' ' << region.tags.size();
        for (const auto& tag : region.tags) output << ' ' << std::quoted(tag);
        output << ' ';
        write_provenance(output, region.provenance);
        output << '\n';
    }
    output << "BOUNDARIES " << boundaries_.size() << '\n';
    for (const auto& [id, boundary] : boundaries_) {
        output << "BOUNDARY " << id.value << ' ' << optional_id(boundary.region) << ' '
               << std::quoted(boundary.semantic_type) << ' ' << std::quoted(boundary.geometry.kind) << ' '
               << std::quoted(boundary.geometry.reference) << ' ';
        write_provenance(output, boundary.provenance);
        output << '\n';
    }
    output << "INTERFACES " << interfaces_.size() << '\n';
    for (const auto& [id, relation] : interfaces_) {
        output << "INTERFACE " << id.value << ' ' << relation.first_region.value << ' '
               << relation.second_region.value << ' ' << std::quoted(relation.semantic_type) << ' ';
        write_provenance(output, relation.provenance);
        output << '\n';
    }
    output << "SOURCES " << sources_.size() << '\n';
    for (const auto& [id, source] : sources_) {
        output << "SOURCE " << id.value << ' ' << optional_id(source.region) << ' '
               << std::quoted(source.semantic_type) << ' ' << std::quoted(source.units) << ' '
               << source.value << ' ';
        write_provenance(output, source.provenance);
        output << '\n';
    }
    output << "LOADS " << loads_.size() << '\n';
    for (const auto& [id, load] : loads_) {
        output << "LOAD " << id.value << ' ' << optional_id(load.region) << ' '
               << std::quoted(load.semantic_type) << ' ' << std::quoted(load.units) << ' '
               << load.value << ' ';
        write_provenance(output, load.provenance);
        output << '\n';
    }
    output << "PARAMETERS " << parameters_.size() << '\n';
    for (const auto& [id, parameter] : parameters_) {
        output << "PARAMETER " << id.value << ' ' << std::quoted(parameter.name) << ' '
               << std::quoted(parameter.units) << ' ' << parameter.value << ' '
               << (parameter.lower_bound.has_value() ? 1U : 0U);
        if (parameter.lower_bound.has_value()) output << ' ' << *parameter.lower_bound;
        output << ' ' << (parameter.upper_bound.has_value() ? 1U : 0U);
        if (parameter.upper_bound.has_value()) output << ' ' << *parameter.upper_bound;
        output << ' ';
        write_provenance(output, parameter.provenance);
        output << '\n';
    }
    output << "FIELDS " << fields_.size() << '\n';
    for (const auto& [id, field] : fields_) {
        output << "FIELD " << id.value << ' ' << static_cast<unsigned>(field.kind) << ' '
               << static_cast<unsigned>(field.association) << ' ' << std::quoted(field.units) << ' '
               << std::quoted(field.coordinate_frame) << ' ' << std::quoted(field.time_or_stage) << ' '
               << optional_id(field.source_study) << ' ' << optional_id(field.sample_domain) << ' '
               << std::quoted(field.provider);
        write_optional_digest(output, field.data_digest);
        output << ' ';
        write_provenance(output, field.provenance);
        output << '\n';
    }
    output << "PROBES " << probes_.size() << '\n';
    for (const auto& [id, probe] : probes_) {
        output << "PROBE " << id.value << ' ' << std::quoted(probe.name) << ' '
               << optional_id(probe.region) << ' ' << optional_id(probe.field) << ' '
               << std::quoted(probe.location_reference) << ' ';
        write_provenance(output, probe.provenance);
        output << '\n';
    }
    output << "MORPHOLOGY " << (morphology_.has_value() ? 1U : 0U) << '\n';
    if (morphology_.has_value()) {
        output << "MORPHOLOGY_DATA " << std::quoted(morphology_->serialize()) << '\n';
    }
    output << "FUNCTIONAL_GENOME " << (functional_genome_.has_value() ? 1U : 0U) << '\n';
    if (functional_genome_.has_value()) {
        output << "FUNCTIONAL_GENOME_DATA " << std::quoted(functional_genome_->serialize()) << '\n';
    }
    output << "DEVELOPMENTAL_PROGRAM " << (developmental_program_.has_value() ? 1U : 0U) << '\n';
    if (developmental_program_.has_value()) {
        output << "DEVELOPMENTAL_PROGRAM_DATA "
               << std::quoted(developmental_program_->serialize()) << '\n';
    }
    output << "DEVELOPMENTAL_TRACE " << (developmental_trace_.has_value() ? 1U : 0U) << '\n';
    if (developmental_trace_.has_value()) {
        output << "DEVELOPMENTAL_TRACE_DATA "
               << std::quoted(developmental_trace_->serialize()) << '\n';
    }
    output << "LINEAGE_GRAPH " << (lineage_graph_.has_value() ? 1U : 0U) << '\n';
    if (lineage_graph_.has_value()) {
        output << "LINEAGE_GRAPH_DATA " << std::quoted(lineage_graph_->serialize()) << '\n';
    }
    output << "MORPHOMETRICS " << (morphometrics_.has_value() ? 1U : 0U) << '\n';
    if (morphometrics_.has_value()) {
        output << "MORPHOMETRICS_DATA " << std::quoted(morphometrics_->serialize()) << '\n';
    }
    output << "SEQUENCE_GENOME " << (sequence_genome_.has_value() ? 1U : 0U) << '\n';
    if (sequence_genome_.has_value()) {
        output << "SEQUENCE_GENOME_DATA " << std::quoted(sequence_genome_->serialize()) << '\n';
    }
    output << "ANATOMY " << (anatomy_.has_value() ? 1U : 0U) << '\n';
    if (anatomy_.has_value()) {
        output << "ANATOMY_DATA " << std::quoted(anatomy_->serialize()) << '\n';
    }
    output << "FIELD_VISUALIZATIONS " << (field_visualizations_.has_value() ? 1U : 0U) << '\n';
    if (field_visualizations_.has_value()) {
        output << "FIELD_VISUALIZATIONS_DATA "
               << std::quoted(field_visualizations_->serialize()) << '\n';
    }
    output << "IMPORT_LEDGER " << (import_ledger_.has_value() ? 1U : 0U) << '\n';
    if (import_ledger_.has_value()) {
        output << "IMPORT_LEDGER_DATA " << std::quoted(import_ledger_->serialize()) << '\n';
    }
    output << "PHENOTYPE_CAPABILITIES " << (phenotype_capabilities_.has_value() ? 1U : 0U) << '\n';
    if (phenotype_capabilities_.has_value()) {
        output << "PHENOTYPE_CAPABILITIES_DATA "
               << std::quoted(phenotype_capabilities_->serialize()) << '\n';
    }
    output << "GENE_REGULATORY_NETWORK " << (gene_regulatory_network_.has_value() ? 1U : 0U) << '\n';
    if (gene_regulatory_network_.has_value()) {
        output << "GENE_REGULATORY_NETWORK_DATA "
               << std::quoted(gene_regulatory_network_->serialize()) << '\n';
    }
    output << "ANATOMY_PROVIDER_MANIFEST "
           << (anatomy_provider_manifest_.has_value() ? 1U : 0U) << '\n';
    if (anatomy_provider_manifest_.has_value()) {
        output << "ANATOMY_PROVIDER_MANIFEST_DATA "
               << std::quoted(anatomy_provider_manifest_->serialize()) << '\n';
    }
    output << "REPLAY_CAPSULES " << (replay_capsules_.has_value() ? 1U : 0U) << '\n';
    if (replay_capsules_.has_value()) {
        output << "REPLAY_CAPSULES_DATA "
               << std::quoted(replay_capsules_->serialize()) << '\n';
    }
    output << "STUDIES " << studies_.size() << '\n';
    for (const auto& [id, study] : studies_) {
        output << "STUDY " << id.value << ' ' << study.source_model_revision.value() << ' ';
        write_id_list(output, study.regions); output << ' ';
        write_id_list(output, study.boundaries); output << ' ';
        write_id_list(output, study.interfaces); output << ' ';
        write_id_list(output, study.sources); output << ' ';
        write_id_list(output, study.loads); output << ' ';
        write_id_list(output, study.parameters); output << ' ';
        output << std::quoted(study.solver_configuration) << ' ' << std::quoted(study.provider) << ' ';
        write_id_list(output, study.requested_outputs); output << ' ';
        output << std::quoted(study.acceptance_metadata);
        write_optional_digest(output, study.study_digest);
        output << ' ';
        write_provenance(output, study.provenance);
        output << '\n';
    }
    output << "RESULT_SETS " << result_sets_.size() << '\n';
    for (const auto& [id, result_set] : result_sets_) {
        output << "RESULT_SET " << id.value << ' ' << result_set.source_study.value << ' ';
        write_id_list(output, result_set.fields); output << ' ';
        write_id_list(output, result_set.probes); output << ' ';
        output << std::quoted(result_set.provider) << ' ' << std::quoted(result_set.solver_version);
        write_optional_digest(output, result_set.source_study_digest);
        write_optional_digest(output, result_set.result_digest);
        output << ' ' << std::quoted(result_set.convergence_evidence) << ' ';
        write_provenance(output, result_set.provenance);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ScientificModel> ScientificModel::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<ScientificModel>::failure(parse_error(
            "scientific model exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = require_record(input, "CARTOGRAPHER_SCIENTIFIC_MODEL"); !result) {
        return core::Result<ScientificModel>::failure(result.error());
    }
    const auto version = read_uint(input, "scientific model schema version");
    if (!version || version.value() == 0U || version.value() > kSchemaVersion) {
        return core::Result<ScientificModel>::failure(Diagnostic(
            ErrorCode::version_mismatch, "unsupported scientific model schema version"));
    }
    ScientificModel model;
    const auto read_count = [&input](std::string_view field) {
        return read_uint(input, field);
    };
    const auto insert_failure = [](const auto& result, std::string_view context)
        -> core::Result<ScientificModel> {
        return core::Result<ScientificModel>::failure(result.error().with_context(std::string(context)));
    };

    if (auto result = require_record(input, "REGIONS"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto region_count = read_count("region count");
    if (!region_count || region_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid region count"));
    for (std::uint64_t index = 0U; index < region_count.value(); ++index) {
        if (auto result = require_record(input, "REGION"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "region id"); const auto parent = read_optional_id<RegionId>(input, "region parent id");
        const auto semantic = read_string(input, "region semantic type", 256U, true);
        const auto geometry_kind = read_string(input, "region geometry kind", 128U);
        const auto geometry_reference = read_string(input, "region geometry reference", kMaxReferenceBytes);
        const auto material = read_string(input, "region material/tissue binding", kMaxReferenceBytes);
        const auto tag_count = read_uint(input, "region tag count");
        if (!id || !parent || !semantic || !geometry_kind || !geometry_reference || !material ||
            !tag_count || tag_count.value() > kMaxTags) return core::Result<ScientificModel>::failure(parse_error("invalid region record"));
        std::vector<std::string> tags; tags.reserve(static_cast<std::size_t>(tag_count.value()));
        for (std::uint64_t tag = 0U; tag < tag_count.value(); ++tag) {
            const auto value = read_string(input, "region tag", 256U, true);
            if (!value) return core::Result<ScientificModel>::failure(value.error());
            tags.push_back(value.value());
        }
        const auto provenance = read_provenance(input);
        if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        Region value{RegionId{id.value()}, semantic.value(),
                      GeometryBinding{geometry_kind.value(), geometry_reference.value()},
                      parent.value(), material.value(), std::move(tags), provenance.value()};
        auto inserted = model.insert_region(std::move(value));
        if (!inserted) return insert_failure(inserted, "region record");
    }
    if (auto result = require_record(input, "BOUNDARIES"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto boundary_count = read_count("boundary count");
    if (!boundary_count || boundary_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid boundary count"));
    for (std::uint64_t index = 0U; index < boundary_count.value(); ++index) {
        if (auto result = require_record(input, "BOUNDARY"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "boundary id"); const auto region = read_optional_id<RegionId>(input, "boundary region id");
        const auto semantic = read_string(input, "boundary semantic type", 256U, true);
        const auto kind = read_string(input, "boundary geometry kind", 128U);
        const auto reference = read_string(input, "boundary geometry reference", kMaxReferenceBytes);
        if (!id || !region || !semantic || !kind || !reference) return core::Result<ScientificModel>::failure(parse_error("invalid boundary record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_boundary(Boundary{BoundaryId{id.value()}, semantic.value(), GeometryBinding{kind.value(), reference.value()}, region.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "boundary record");
    }
    if (auto result = require_record(input, "INTERFACES"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto interface_count = read_count("interface count");
    if (!interface_count || interface_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid interface count"));
    for (std::uint64_t index = 0U; index < interface_count.value(); ++index) {
        if (auto result = require_record(input, "INTERFACE"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "interface id"); const auto first = read_uint(input, "interface first region"); const auto second = read_uint(input, "interface second region");
        const auto semantic = read_string(input, "interface semantic type", 256U, true);
        if (!id || !first || !second || !semantic || first.value() == 0U || second.value() == 0U) return core::Result<ScientificModel>::failure(parse_error("invalid interface record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_interface(Interface{InterfaceId{id.value()}, semantic.value(), RegionId{first.value()}, RegionId{second.value()}, provenance.value()});
        if (!inserted) return insert_failure(inserted, "interface record");
    }
    if (auto result = require_record(input, "SOURCES"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto source_count = read_count("source count");
    if (!source_count || source_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid source count"));
    for (std::uint64_t index = 0U; index < source_count.value(); ++index) {
        if (auto result = require_record(input, "SOURCE"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "source id"); const auto region = read_optional_id<RegionId>(input, "source region id");
        const auto semantic = read_string(input, "source semantic type", 256U, true); const auto units = read_string(input, "source units", 128U); const auto value = read_double(input, "source value");
        if (!id || !region || !semantic || !units || !value) return core::Result<ScientificModel>::failure(parse_error("invalid source record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_source(Source{SourceId{id.value()}, semantic.value(), region.value(), units.value(), value.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "source record");
    }
    if (auto result = require_record(input, "LOADS"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto load_count = read_count("load count");
    if (!load_count || load_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid load count"));
    for (std::uint64_t index = 0U; index < load_count.value(); ++index) {
        if (auto result = require_record(input, "LOAD"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "load id"); const auto region = read_optional_id<RegionId>(input, "load region id");
        const auto semantic = read_string(input, "load semantic type", 256U, true); const auto units = read_string(input, "load units", 128U); const auto value = read_double(input, "load value");
        if (!id || !region || !semantic || !units || !value) return core::Result<ScientificModel>::failure(parse_error("invalid load record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_load(Load{LoadId{id.value()}, semantic.value(), region.value(), units.value(), value.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "load record");
    }
    if (auto result = require_record(input, "PARAMETERS"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto parameter_count = read_count("parameter count");
    if (!parameter_count || parameter_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid parameter count"));
    for (std::uint64_t index = 0U; index < parameter_count.value(); ++index) {
        if (auto result = require_record(input, "PARAMETER"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "parameter id"); const auto name = read_string(input, "parameter name", 256U, true); const auto units = read_string(input, "parameter units", 128U); const auto value = read_double(input, "parameter value"); const auto has_lower = read_uint(input, "parameter lower flag");
        if (!id || !name || !units || !value || !has_lower || has_lower.value() > 1U) return core::Result<ScientificModel>::failure(parse_error("invalid parameter record"));
        std::optional<double> lower; if (has_lower.value() != 0U) { const auto parsed = read_double(input, "parameter lower bound"); if (!parsed) return core::Result<ScientificModel>::failure(parsed.error()); lower = parsed.value(); }
        const auto has_upper = read_uint(input, "parameter upper flag"); if (!has_upper || has_upper.value() > 1U) return core::Result<ScientificModel>::failure(parse_error("invalid parameter upper flag"));
        std::optional<double> upper; if (has_upper.value() != 0U) { const auto parsed = read_double(input, "parameter upper bound"); if (!parsed) return core::Result<ScientificModel>::failure(parsed.error()); upper = parsed.value(); }
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_parameter(Parameter{ParameterId{id.value()}, name.value(), units.value(), value.value(), lower, upper, provenance.value()});
        if (!inserted) return insert_failure(inserted, "parameter record");
    }
    if (auto result = require_record(input, "FIELDS"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto field_count = read_count("field count");
    if (!field_count || field_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid field count"));
    for (std::uint64_t index = 0U; index < field_count.value(); ++index) {
        if (auto result = require_record(input, "FIELD"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "field id"); const auto kind = read_uint(input, "field kind"); const auto association = read_uint(input, "field association");
        const auto units = read_string(input, "field units", 128U); const auto frame = read_string(input, "field coordinate frame", 256U); const auto stage = read_string(input, "field time/stage", 256U);
        const auto study = read_optional_id<StudyId>(input, "field source study"); const auto domain = read_optional_id<RegionId>(input, "field sample domain"); const auto provider = read_string(input, "field provider", 256U);
        if (!id || !kind || !association || !units || !frame || !stage || !study || !domain || !provider || kind.value() > static_cast<unsigned>(FieldKind::categorical) || association.value() > static_cast<unsigned>(Association::sample_set)) return core::Result<ScientificModel>::failure(parse_error("invalid field record"));
        const auto digest = read_optional_digest(input, "field data"); if (!digest) return core::Result<ScientificModel>::failure(digest.error());
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_field(Field{FieldId{id.value()}, static_cast<FieldKind>(kind.value()), static_cast<Association>(association.value()), units.value(), frame.value(), stage.value(), study.value(), domain.value(), provider.value(), digest.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "field record");
    }
    if (auto result = require_record(input, "PROBES"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto probe_count = read_count("probe count");
    if (!probe_count || probe_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid probe count"));
    for (std::uint64_t index = 0U; index < probe_count.value(); ++index) {
        if (auto result = require_record(input, "PROBE"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "probe id"); const auto name = read_string(input, "probe name", 256U, true); const auto region = read_optional_id<RegionId>(input, "probe region"); const auto field = read_optional_id<FieldId>(input, "probe field"); const auto location = read_string(input, "probe location reference", kMaxReferenceBytes, true);
        if (!id || !name || !region || !field || !location) return core::Result<ScientificModel>::failure(parse_error("invalid probe record"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_probe(Probe{ProbeId{id.value()}, name.value(), region.value(), field.value(), location.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "probe record");
    }
    if (version.value() >= 2U) {
        if (auto result = require_record(input, "MORPHOLOGY"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto morphology_present = read_uint(input, "morphology presence");
        if (!morphology_present || morphology_present.value() > 1U) return core::Result<ScientificModel>::failure(parse_error("invalid morphology presence flag"));
        if (morphology_present.value() != 0U) {
            if (auto result = require_record(input, "MORPHOLOGY_DATA"); !result) return core::Result<ScientificModel>::failure(result.error());
            std::string encoded;
            if (!(input >> std::quoted(encoded))) return core::Result<ScientificModel>::failure(parse_error("invalid morphology data"));
            const auto morphology = MorphologyModel::deserialize(encoded);
            if (!morphology) return core::Result<ScientificModel>::failure(morphology.error().with_context("serialized morphology"));
            model.morphology_ = morphology.value();
        }
        if (auto result = require_record(input, "FUNCTIONAL_GENOME"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto genome_present = read_uint(input, "functional genome presence");
        if (!genome_present || genome_present.value() > 1U) return core::Result<ScientificModel>::failure(parse_error("invalid functional genome presence flag"));
        if (genome_present.value() != 0U) {
            if (auto result = require_record(input, "FUNCTIONAL_GENOME_DATA"); !result) return core::Result<ScientificModel>::failure(result.error());
            std::string encoded;
            if (!(input >> std::quoted(encoded))) return core::Result<ScientificModel>::failure(parse_error("invalid functional genome data"));
            const auto genome = FunctionalGenome::deserialize(encoded);
            if (!genome) return core::Result<ScientificModel>::failure(genome.error().with_context("serialized functional genome"));
            model.functional_genome_ = genome.value();
        }
    }
    if (version.value() >= 3U) {
        if (auto result = require_record(input, "DEVELOPMENTAL_PROGRAM"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto program_present = read_uint(input, "developmental program presence");
        if (!program_present || program_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid developmental program presence flag"));
        }
        if (program_present.value() != 0U) {
            if (auto result = require_record(input, "DEVELOPMENTAL_PROGRAM_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid developmental program data"));
            }
            const auto program = DevelopmentalProgram::deserialize(encoded);
            if (!program) {
                return core::Result<ScientificModel>::failure(
                    program.error().with_context("serialized developmental program"));
            }
            model.developmental_program_ = program.value();
        }
        if (auto result = require_record(input, "DEVELOPMENTAL_TRACE"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto trace_present = read_uint(input, "developmental trace presence");
        if (!trace_present || trace_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid developmental trace presence flag"));
        }
        if (trace_present.value() != 0U) {
            if (auto result = require_record(input, "DEVELOPMENTAL_TRACE_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid developmental trace data"));
            }
            const auto trace = DevelopmentalTrace::deserialize(encoded);
            if (!trace) {
                return core::Result<ScientificModel>::failure(
                    trace.error().with_context("serialized developmental trace"));
            }
            model.developmental_trace_ = trace.value();
        }
        if (auto result = require_record(input, "LINEAGE_GRAPH"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto lineage_present = read_uint(input, "lineage graph presence");
        if (!lineage_present || lineage_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid lineage graph presence flag"));
        }
        if (lineage_present.value() != 0U) {
            if (auto result = require_record(input, "LINEAGE_GRAPH_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid lineage graph data"));
            }
            const auto lineage = LineageGraph::deserialize(encoded);
            if (!lineage) {
                return core::Result<ScientificModel>::failure(
                    lineage.error().with_context("serialized lineage graph"));
            }
            model.lineage_graph_ = lineage.value();
        }
    }
    if (version.value() >= 4U) {
        if (auto result = require_record(input, "MORPHOMETRICS"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto morphometrics_present = read_uint(input, "morphometrics presence");
        if (!morphometrics_present || morphometrics_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid morphometrics presence flag"));
        }
        if (morphometrics_present.value() != 0U) {
            if (auto result = require_record(input, "MORPHOMETRICS_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid morphometrics data"));
            }
            const auto morphometrics = MorphometricModel::deserialize(encoded);
            if (!morphometrics) {
                return core::Result<ScientificModel>::failure(
                    morphometrics.error().with_context("serialized morphometrics"));
            }
            model.morphometrics_ = morphometrics.value();
        }
    }
    if (version.value() >= 5U) {
        if (auto result = require_record(input, "SEQUENCE_GENOME"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto sequence_present = read_uint(input, "sequence genome presence");
        if (!sequence_present || sequence_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid sequence genome presence flag"));
        }
        if (sequence_present.value() != 0U) {
            if (auto result = require_record(input, "SEQUENCE_GENOME_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid sequence genome data"));
            }
            const auto sequence = SequenceGenome::deserialize(encoded);
            if (!sequence) {
                return core::Result<ScientificModel>::failure(
                    sequence.error().with_context("serialized sequence genome"));
            }
            model.sequence_genome_ = sequence.value();
        }
    }
    if (version.value() >= 6U) {
        if (auto result = require_record(input, "ANATOMY"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto anatomy_present = read_uint(input, "anatomy presence");
        if (!anatomy_present || anatomy_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid anatomy presence flag"));
        }
        if (anatomy_present.value() != 0U) {
            if (auto result = require_record(input, "ANATOMY_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error("invalid anatomy data"));
            }
            const auto anatomy = AnatomyModel::deserialize(encoded);
            if (!anatomy) {
                return core::Result<ScientificModel>::failure(
                    anatomy.error().with_context("serialized anatomy"));
            }
            model.anatomy_ = anatomy.value();
        }
    }
    if (version.value() >= 7U) {
        if (auto result = require_record(input, "FIELD_VISUALIZATIONS"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto visualizations_present = read_uint(input, "field visualizations presence");
        if (!visualizations_present || visualizations_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid field visualizations presence flag"));
        }
        if (visualizations_present.value() != 0U) {
            if (auto result = require_record(input, "FIELD_VISUALIZATIONS_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid field visualizations data"));
            }
            const auto visualizations = FieldVisualizationModel::deserialize(encoded);
            if (!visualizations) {
                return core::Result<ScientificModel>::failure(
                    visualizations.error().with_context("serialized field visualizations"));
            }
            model.field_visualizations_ = visualizations.value();
        }
    }
    if (version.value() >= 8U) {
        if (auto result = require_record(input, "IMPORT_LEDGER"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto ledger_present = read_uint(input, "import ledger presence");
        if (!ledger_present || ledger_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid import ledger presence flag"));
        }
        if (ledger_present.value() != 0U) {
            if (auto result = require_record(input, "IMPORT_LEDGER_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error("invalid import ledger data"));
            }
            const auto ledger = ScientificImportLedger::deserialize(encoded);
            if (!ledger) {
                return core::Result<ScientificModel>::failure(
                    ledger.error().with_context("serialized import ledger"));
            }
            model.import_ledger_ = ledger.value();
        }
    }
    if (version.value() >= 9U) {
        if (auto result = require_record(input, "PHENOTYPE_CAPABILITIES"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto phenotype_present = read_uint(input, "phenotype capabilities presence");
        if (!phenotype_present || phenotype_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid phenotype capabilities presence flag"));
        }
        if (phenotype_present.value() != 0U) {
            if (auto result = require_record(input, "PHENOTYPE_CAPABILITIES_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid phenotype capabilities data"));
            }
            const auto phenotype = PhenotypeCapabilityModel::deserialize(encoded);
            if (!phenotype) {
                return core::Result<ScientificModel>::failure(
                    phenotype.error().with_context("serialized phenotype capabilities"));
            }
            model.phenotype_capabilities_ = phenotype.value();
        }
    }
    if (version.value() >= 10U) {
        if (auto result = require_record(input, "GENE_REGULATORY_NETWORK"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto network_present = read_uint(input, "gene regulatory network presence");
        if (!network_present || network_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid gene regulatory network presence flag"));
        }
        if (network_present.value() != 0U) {
            if (auto result = require_record(input, "GENE_REGULATORY_NETWORK_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid gene regulatory network data"));
            }
            const auto network = GeneRegulatoryNetwork::deserialize(encoded);
            if (!network) {
                return core::Result<ScientificModel>::failure(
                    network.error().with_context("serialized gene regulatory network"));
            }
            model.gene_regulatory_network_ = network.value();
        }
    }
    if (version.value() >= 11U) {
        if (auto result = require_record(input, "ANATOMY_PROVIDER_MANIFEST"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto manifest_present = read_uint(input, "anatomy provider manifest presence");
        if (!manifest_present || manifest_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid anatomy provider manifest presence flag"));
        }
        if (manifest_present.value() != 0U) {
            if (auto result = require_record(input, "ANATOMY_PROVIDER_MANIFEST_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid anatomy provider manifest data"));
            }
            const auto manifest = AnatomyProviderManifest::deserialize(encoded);
            if (!manifest) {
                return core::Result<ScientificModel>::failure(
                    manifest.error().with_context("serialized anatomy provider manifest"));
            }
            model.anatomy_provider_manifest_ = manifest.value();
        }
    }
    if (version.value() >= 12U) {
        if (auto result = require_record(input, "REPLAY_CAPSULES"); !result) {
            return core::Result<ScientificModel>::failure(result.error());
        }
        const auto replay_present = read_uint(input, "scientific replay capsules presence");
        if (!replay_present || replay_present.value() > 1U) {
            return core::Result<ScientificModel>::failure(parse_error(
                "invalid scientific replay capsules presence flag"));
        }
        if (replay_present.value() != 0U) {
            if (auto result = require_record(input, "REPLAY_CAPSULES_DATA"); !result) {
                return core::Result<ScientificModel>::failure(result.error());
            }
            std::string encoded;
            if (!(input >> std::quoted(encoded))) {
                return core::Result<ScientificModel>::failure(parse_error(
                    "invalid scientific replay capsules data"));
            }
            const auto replay = ScientificReplayCapsuleModel::deserialize(encoded);
            if (!replay) {
                return core::Result<ScientificModel>::failure(
                    replay.error().with_context("serialized scientific replay capsules"));
            }
            model.replay_capsules_ = replay.value();
        }
    }
    if (auto result = require_record(input, "STUDIES"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto study_count = read_count("study count");
    if (!study_count || study_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid study count"));
    for (std::uint64_t index = 0U; index < study_count.value(); ++index) {
        if (auto result = require_record(input, "STUDY"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "study id"); const auto revision = read_uint(input, "study source revision");
        if (!id || !revision) return core::Result<ScientificModel>::failure(parse_error("invalid study identity"));
        const auto regions = read_id_list<RegionId>(input, "study regions"); const auto boundaries = read_id_list<BoundaryId>(input, "study boundaries"); const auto interfaces = read_id_list<InterfaceId>(input, "study interfaces"); const auto sources = read_id_list<SourceId>(input, "study sources"); const auto loads = read_id_list<LoadId>(input, "study loads"); const auto parameters = read_id_list<ParameterId>(input, "study parameters");
        const auto solver = read_string(input, "study solver configuration", kMaxTextBytes); const auto provider = read_string(input, "study provider", 256U); const auto outputs = read_id_list<FieldId>(input, "study requested outputs"); const auto metadata = read_string(input, "study acceptance metadata", kMaxTextBytes);
        if (!regions || !boundaries || !interfaces || !sources || !loads || !parameters || !solver || !provider || !outputs || !metadata) return core::Result<ScientificModel>::failure(parse_error("invalid study record"));
        const auto digest = read_optional_digest(input, "study"); if (!digest) return core::Result<ScientificModel>::failure(digest.error());
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_study(Study{StudyId{id.value()}, core::Revision{revision.value()}, regions.value(), boundaries.value(), interfaces.value(), sources.value(), loads.value(), parameters.value(), solver.value(), provider.value(), outputs.value(), metadata.value(), digest.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "study record");
    }
    if (auto result = require_record(input, "RESULT_SETS"); !result) return core::Result<ScientificModel>::failure(result.error());
    const auto result_count = read_count("result set count");
    if (!result_count || result_count.value() > kMaxCollectionEntries) return core::Result<ScientificModel>::failure(parse_error("invalid result set count"));
    for (std::uint64_t index = 0U; index < result_count.value(); ++index) {
        if (auto result = require_record(input, "RESULT_SET"); !result) return core::Result<ScientificModel>::failure(result.error());
        const auto id = read_uint(input, "result set id"); const auto study = read_uint(input, "result set source study");
        if (!id || !study || study.value() == 0U) return core::Result<ScientificModel>::failure(parse_error("invalid result set identity"));
        const auto fields = read_id_list<FieldId>(input, "result set fields"); const auto probes = read_id_list<ProbeId>(input, "result set probes"); const auto provider = read_string(input, "result set provider", 256U); const auto solver = read_string(input, "result set solver version", 256U);
        if (!fields || !probes || !provider || !solver) return core::Result<ScientificModel>::failure(parse_error("invalid result set record"));
        const auto source_digest = read_optional_digest(input, "result set source study"); const auto result_digest = read_optional_digest(input, "result set result"); const auto evidence = read_string(input, "result set convergence evidence", kMaxTextBytes);
        if (!source_digest || !result_digest || !evidence) return core::Result<ScientificModel>::failure(parse_error("invalid result set digest/evidence"));
        const auto provenance = read_provenance(input); if (!provenance) return core::Result<ScientificModel>::failure(provenance.error());
        auto inserted = model.insert_result_set(ResultSet{ResultSetId{id.value()}, StudyId{study.value()}, fields.value(), probes.value(), provider.value(), solver.value(), source_digest.value(), result_digest.value(), evidence.value(), provenance.value()});
        if (!inserted) return insert_failure(inserted, "result set record");
    }
    if (auto result = require_record(input, "END"); !result) return core::Result<ScientificModel>::failure(result.error());
    std::string trailing;
    if (input >> trailing) return core::Result<ScientificModel>::failure(parse_error("scientific model contains trailing data"));
    if (auto result = model.validate(); !result) return core::Result<ScientificModel>::failure(result.error());
    return core::Result<ScientificModel>::success(std::move(model));
}

} // namespace carto::scientific
