#include <carto/project/presentation_catalog.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::project {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PRESENTATION_CATALOG";
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxSourceValue = 1.0e9;

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic validation(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

bool safe_identifier(std::string_view value, std::size_t maximum, bool optional = false) {
    if (optional && value.empty()) return true;
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
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

bool read_enum(std::istream& input, unsigned maximum, unsigned& value) {
    return read_unsigned(input, value) && value <= maximum;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t parsed = 0U;
    if (!read_unsigned(input, parsed) || parsed > maximum ||
        parsed > std::numeric_limits<std::size_t>::max()) return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool expect(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

} // namespace

core::Result<void> AuthoredCamera::validate() const {
    const bool valid_projection = projection == CameraProjection::perspective ||
        projection == CameraProjection::orthographic;
    const bool valid_purpose = static_cast<unsigned>(review_purpose) <=
        static_cast<unsigned>(CameraReviewPurpose::screenshot_evidence);
    if (!object || !safe_identifier(identity, 128U) ||
        !safe_identifier(bookmark_identity, 128U) ||
        !safe_identifier(reference_image_identity, 128U, true) || !valid_projection ||
        !valid_purpose || revision.exhausted() || !std::isfinite(vertical_fov_radians) ||
        !std::isfinite(orthographic_height) || !std::isfinite(near_plane) ||
        !std::isfinite(far_plane) || vertical_fov_radians <= 0.01 ||
        vertical_fov_radians >= kPi - 0.01 || orthographic_height <= 0.0 ||
        orthographic_height > kMaxSourceValue || near_plane <= 0.0 ||
        far_plane <= near_plane || far_plane > kMaxSourceValue) {
        return core::Result<void>::failure(invalid(
            "authored camera identity, projection, range, or revision is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> AuthoredLight::validate() const {
    const auto finite_color = [](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= kMaxSourceValue;
    };
    const bool valid_kind = static_cast<unsigned>(kind) <=
        static_cast<unsigned>(AuthoredLightKind::area);
    const bool valid_normal = static_cast<unsigned>(normal_state) <=
        static_cast<unsigned>(LightNormalState::intermittent);
    const bool valid_failure = static_cast<unsigned>(failure_state) <=
        static_cast<unsigned>(LightFailureState::intermittent);
    if (!object || !safe_identifier(identity, 128U) ||
        !safe_identifier(semantic_class, 128U, true) ||
        !safe_identifier(fixture_owner, 128U, true) ||
        !safe_identifier(power_owner, 128U, true) ||
        !safe_identifier(switch_owner, 128U, true)) {
        return core::Result<void>::failure(invalid(
            "authored light identity or semantic references are invalid"));
    }
    if (!valid_kind || !valid_normal || !valid_failure || revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "authored light enum or revision is invalid"));
    }
    if (!finite_color(linear_color.x) || !finite_color(linear_color.y) ||
        !finite_color(linear_color.z) ||
        !std::isfinite(intensity) || intensity <= 0.0 || intensity > kMaxSourceValue ||
        !std::isfinite(range) || !std::isfinite(inner_cone_radians) ||
        !std::isfinite(outer_cone_radians) || !std::isfinite(area_width) ||
        !std::isfinite(area_height)) {
        return core::Result<void>::failure(invalid(
            "authored light photometry is invalid"));
    }
    if (color_temperature_kelvin.has_value() &&
        (!std::isfinite(*color_temperature_kelvin) ||
         *color_temperature_kelvin < 1'000.0 || *color_temperature_kelvin > 40'000.0)) {
        return core::Result<void>::failure(invalid(
            "authored light color temperature is outside its bounded range"));
    }
    switch (kind) {
    case AuthoredLightKind::directional:
        if (range == 0.0 && inner_cone_radians == 0.0 && outer_cone_radians == 0.0 &&
            area_width == 0.0 && area_height == 0.0) return core::Result<void>::success();
        break;
    case AuthoredLightKind::point:
        if (range > 0.0 && range <= kMaxSourceValue &&
            inner_cone_radians == 0.0 && outer_cone_radians == 0.0 &&
            area_width == 0.0 && area_height == 0.0) return core::Result<void>::success();
        break;
    case AuthoredLightKind::spot:
        if (range > 0.0 && range <= kMaxSourceValue && inner_cone_radians >= 0.0 &&
            inner_cone_radians <= outer_cone_radians && outer_cone_radians < kPi / 2.0 &&
            area_width == 0.0 && area_height == 0.0) return core::Result<void>::success();
        break;
    case AuthoredLightKind::area:
        if (range > 0.0 && range <= kMaxSourceValue &&
            inner_cone_radians == 0.0 && outer_cone_radians == 0.0 &&
            area_width > 0.0 && area_width <= kMaxSourceValue &&
            area_height > 0.0 && area_height <= kMaxSourceValue) {
            return core::Result<void>::success();
        }
        break;
    }
    return core::Result<void>::failure(validation(
        "authored light parameters contradict its analytic light kind"));
}

core::Result<void> PresentationCatalog::insert_camera(AuthoredCamera camera) {
    if (auto result = camera.validate(); !result) return result;
    if (cameras_.size() >= kMaxRecords) {
        return core::Result<void>::failure(validation("camera catalog is at capacity"));
    }
    const auto identity = camera.object;
    const auto [iterator, inserted] = cameras_.emplace(identity, std::move(camera));
    static_cast<void>(iterator);
    if (!inserted) return core::Result<void>::failure(invalid("camera object identity is duplicated"));
    return core::Result<void>::success();
}

core::Result<void> PresentationCatalog::insert_light(AuthoredLight light) {
    if (auto result = light.validate(); !result) return result;
    if (lights_.size() >= kMaxRecords) {
        return core::Result<void>::failure(validation("light catalog is at capacity"));
    }
    const auto identity = light.object;
    const auto [iterator, inserted] = lights_.emplace(identity, std::move(light));
    static_cast<void>(iterator);
    if (!inserted) return core::Result<void>::failure(invalid("light object identity is duplicated"));
    return core::Result<void>::success();
}

core::Result<void> PresentationCatalog::validate() const {
    if (cameras_.size() > kMaxRecords || lights_.size() > kMaxRecords) {
        return core::Result<void>::failure(validation(
            "presentation catalog exceeds a bounded collection size"));
    }
    std::size_t active_cameras = 0U;
    for (const auto& [identity, camera] : cameras_) {
        if (identity != camera.object) return core::Result<void>::failure(validation(
            "camera map identity does not match its source record"));
        if (auto result = camera.validate(); !result) return result;
        active_cameras += camera.active_for_review ? 1U : 0U;
    }
    if (active_cameras > 1U) return core::Result<void>::failure(validation(
        "presentation catalog may have at most one active review camera"));
    for (const auto& [identity, light] : lights_) {
        if (identity != light.object) return core::Result<void>::failure(validation(
            "light map identity does not match its source record"));
        if (auto result = light.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string PresentationCatalog::serialize() const {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << kMagic << ' ' << kSchemaVersion << '\n';
    out << "CAMERAS " << cameras_.size() << '\n';
    for (const auto& [identity, camera] : cameras_) {
        static_cast<void>(identity);
        out << "CAMERA " << camera.object.value << ' ' << camera.revision.value() << ' '
            << static_cast<unsigned>(camera.projection) << ' '
            << static_cast<unsigned>(camera.review_purpose) << ' '
            << (camera.active_for_review ? 1U : 0U) << ' '
            << camera.vertical_fov_radians << ' ' << camera.orthographic_height << ' '
            << camera.near_plane << ' ' << camera.far_plane << ' '
            << std::quoted(camera.identity) << ' ' << std::quoted(camera.bookmark_identity)
            << ' ' << std::quoted(camera.reference_image_identity) << '\n';
    }
    out << "LIGHTS " << lights_.size() << '\n';
    for (const auto& [identity, light] : lights_) {
        static_cast<void>(identity);
        out << "LIGHT " << light.object.value << ' ' << light.revision.value() << ' '
            << static_cast<unsigned>(light.kind) << ' '
            << static_cast<unsigned>(light.normal_state) << ' '
            << static_cast<unsigned>(light.failure_state) << ' '
            << (light.color_temperature_kelvin.has_value() ? 1U : 0U) << ' '
            << light.color_temperature_kelvin.value_or(0.0) << ' '
            << light.linear_color.x << ' ' << light.linear_color.y << ' '
            << light.linear_color.z << ' ' << light.intensity << ' ' << light.range << ' '
            << light.inner_cone_radians << ' ' << light.outer_cone_radians << ' '
            << light.area_width << ' ' << light.area_height << ' '
            << (light.casts_shadows ? 1U : 0U) << ' '
            << (light.flicker_eligible ? 1U : 0U) << ' '
            << std::quoted(light.identity) << ' ' << std::quoted(light.semantic_class) << ' '
            << std::quoted(light.fixture_owner) << ' ' << std::quoted(light.power_owner) << ' '
            << std::quoted(light.switch_owner) << '\n';
    }
    out << "END\n";
    return out.str();
}

core::Result<assets::Sha256Digest> PresentationCatalog::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<PresentationCatalog> PresentationCatalog::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PresentationCatalog>::failure(invalid(
            "presentation catalog is empty or exceeds its serialized size bound"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!expect(input, kMagic) || !read_unsigned(input, version) || version != kSchemaVersion) {
        return core::Result<PresentationCatalog>::failure({
            core::ErrorCode::version_mismatch, "presentation catalog version is unsupported"});
    }
    PresentationCatalog catalog;
    std::size_t count = 0U;
    if (!expect(input, "CAMERAS") || !read_count(input, kMaxRecords, count)) {
        return core::Result<PresentationCatalog>::failure(validation(
            "presentation camera count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        AuthoredCamera camera;
        std::uint64_t object = 0U;
        std::uint64_t revision = 0U;
        unsigned projection = 0U;
        unsigned purpose = 0U;
        if (!expect(input, "CAMERA") || !read_unsigned(input, object) ||
            !read_unsigned(input, revision) ||
            !read_enum(input, static_cast<unsigned>(CameraProjection::orthographic), projection) ||
            !read_enum(input, static_cast<unsigned>(CameraReviewPurpose::screenshot_evidence), purpose) ||
            !read_bool(input, camera.active_for_review) ||
            !(input >> camera.vertical_fov_radians >> camera.orthographic_height >>
              camera.near_plane >> camera.far_plane >> std::quoted(camera.identity) >>
              std::quoted(camera.bookmark_identity) >> std::quoted(camera.reference_image_identity))) {
            return core::Result<PresentationCatalog>::failure(validation(
                "presentation camera record is malformed"));
        }
        camera.object = scene::ObjectId{object};
        camera.revision = core::Revision{revision};
        camera.projection = static_cast<CameraProjection>(projection);
        camera.review_purpose = static_cast<CameraReviewPurpose>(purpose);
        if (auto result = catalog.insert_camera(std::move(camera)); !result) {
            return core::Result<PresentationCatalog>::failure(result.error());
        }
    }
    if (!expect(input, "LIGHTS") || !read_count(input, kMaxRecords, count)) {
        return core::Result<PresentationCatalog>::failure(validation(
            "presentation light count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        AuthoredLight light;
        std::uint64_t object = 0U;
        std::uint64_t revision = 0U;
        unsigned kind = 0U;
        unsigned normal = 0U;
        unsigned failure = 0U;
        bool has_temperature = false;
        double temperature = 0.0;
        if (!expect(input, "LIGHT") || !read_unsigned(input, object) ||
            !read_unsigned(input, revision) ||
            !read_enum(input, static_cast<unsigned>(AuthoredLightKind::area), kind) ||
            !read_enum(input, static_cast<unsigned>(LightNormalState::intermittent), normal) ||
            !read_enum(input, static_cast<unsigned>(LightFailureState::intermittent), failure) ||
            !read_bool(input, has_temperature) ||
            !(input >> temperature >> light.linear_color.x >> light.linear_color.y >>
              light.linear_color.z >> light.intensity >> light.range >> light.inner_cone_radians >>
              light.outer_cone_radians >> light.area_width >> light.area_height) ||
            !read_bool(input, light.casts_shadows) || !read_bool(input, light.flicker_eligible) ||
            !(input >> std::quoted(light.identity) >> std::quoted(light.semantic_class) >>
              std::quoted(light.fixture_owner) >> std::quoted(light.power_owner) >>
              std::quoted(light.switch_owner))) {
            return core::Result<PresentationCatalog>::failure(validation(
                "presentation light record is malformed"));
        }
        light.object = scene::ObjectId{object};
        light.revision = core::Revision{revision};
        light.kind = static_cast<AuthoredLightKind>(kind);
        light.normal_state = static_cast<LightNormalState>(normal);
        light.failure_state = static_cast<LightFailureState>(failure);
        if (has_temperature) {
            light.color_temperature_kelvin = temperature;
        } else if (temperature != 0.0) {
            return core::Result<PresentationCatalog>::failure(validation(
                "presentation light has unbound color-temperature data"));
        }
        if (auto result = catalog.insert_light(std::move(light)); !result) {
            return core::Result<PresentationCatalog>::failure(result.error());
        }
    }
    if (!expect(input, "END")) return core::Result<PresentationCatalog>::failure(validation(
        "presentation catalog terminal record is missing"));
    std::string trailing;
    if (input >> trailing) return core::Result<PresentationCatalog>::failure(validation(
        "presentation catalog contains trailing data"));
    if (auto result = catalog.validate(); !result) {
        return core::Result<PresentationCatalog>::failure(result.error());
    }
    return core::Result<PresentationCatalog>::success(std::move(catalog));
}

} // namespace carto::project
