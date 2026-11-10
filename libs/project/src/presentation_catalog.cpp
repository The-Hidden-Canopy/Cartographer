#include <carto/project/presentation_catalog.hpp>
#include <carto/assets/texture_source.hpp>
#include <carto/core/filesystem.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
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
constexpr std::uint32_t kMaxReferenceImageDimension = 65'536U;
constexpr std::uint64_t kMaxReferenceImagePixels = 268'435'456ULL;
constexpr double kReferenceBasisTolerance = 1.0e-6;

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

bool safe_project_locator(std::string_view path) {
    if (path.empty() || path.size() > assets::SourceImportRecord::kMaxRelativePathBytes ||
        path.front() == '/' || path.front() == '\\' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) return false;
    if (std::any_of(path.begin(), path.end(), [](unsigned char byte) {
            return byte < 0x20U || byte == 0x7fU;
        })) return false;
    std::size_t begin = 0U;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view component = end == std::string_view::npos
            ? path.substr(begin) : path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ') return false;
        std::string device_base(component.substr(0U, component.find('.')));
        std::transform(device_base.begin(), device_base.end(), device_base.begin(),
            [](unsigned char byte) {
                return static_cast<char>(byte >= 'a' && byte <= 'z'
                    ? byte - ('a' - 'A') : byte);
            });
        const bool numbered_device = device_base.size() == 4U &&
            (device_base.starts_with("COM") || device_base.starts_with("LPT")) &&
            device_base[3U] >= '1' && device_base[3U] <= '9';
        if (device_base == "CON" || device_base == "PRN" || device_base == "AUX" ||
            device_base == "NUL" || device_base == "CONIN$" ||
            device_base == "CONOUT$" || numbered_device) return false;
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return true;
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

bool within_source_bounds(core::Vec3d value) {
    return value.finite() && std::abs(value.x) <= kMaxSourceValue &&
        std::abs(value.y) <= kMaxSourceValue && std::abs(value.z) <= kMaxSourceValue;
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
        far_plane <= near_plane || far_plane > kMaxSourceValue ||
        !std::isfinite(exposure_ev) || std::abs(exposure_ev) > 32.0 ||
        static_cast<unsigned>(tone_mode) > static_cast<unsigned>(render::ToneMapMode::linear_diagnostic)) {
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

core::Result<void> ReferenceImageCalibration::validate(
    std::uint32_t image_width_pixels,
    std::uint32_t image_height_pixels) const {
    if (image_width_pixels == 0U || image_height_pixels == 0U ||
        !first_pixel.finite() || !second_pixel.finite() ||
        first_pixel.x < 0.0 || first_pixel.y < 0.0 ||
        second_pixel.x < 0.0 || second_pixel.y < 0.0 ||
        first_pixel.x >= static_cast<double>(image_width_pixels) ||
        second_pixel.x >= static_cast<double>(image_width_pixels) ||
        first_pixel.y >= static_cast<double>(image_height_pixels) ||
        second_pixel.y >= static_cast<double>(image_height_pixels) ||
        !std::isfinite(known_distance_meters) || known_distance_meters <= 0.0 ||
        known_distance_meters > kMaxSourceValue) {
        return core::Result<void>::failure(invalid(
            "reference-image calibration anchors or known distance are invalid"));
    }
    const double pixel_distance = std::hypot(
        second_pixel.x - first_pixel.x, second_pixel.y - first_pixel.y);
    if (!std::isfinite(pixel_distance) || pixel_distance <= 1.0e-9) {
        return core::Result<void>::failure(validation(
            "reference-image calibration anchors must be distinct"));
    }
    const double calibrated_width = known_distance_meters /
        pixel_distance * static_cast<double>(image_width_pixels);
    if (!std::isfinite(calibrated_width) || calibrated_width <= 0.0 ||
        calibrated_width > kMaxSourceValue) {
        return core::Result<void>::failure(validation(
            "reference-image calibration produces an out-of-range plane width"));
    }
    return core::Result<void>::success();
}

core::Result<double> ReferenceImageCalibration::calibrated_width_meters(
    std::uint32_t image_width_pixels,
    std::uint32_t image_height_pixels) const {
    if (auto result = validate(image_width_pixels, image_height_pixels); !result) {
        return core::Result<double>::failure(result.error());
    }
    const double pixel_distance = std::hypot(
        second_pixel.x - first_pixel.x, second_pixel.y - first_pixel.y);
    return core::Result<double>::success(known_distance_meters /
        pixel_distance * static_cast<double>(image_width_pixels));
}

core::Result<void> ReferenceImagePlane::validate() const {
    const bool valid_view = static_cast<unsigned>(view) <=
        static_cast<unsigned>(ReferencePlaneView::camera_pinned);
    const bool orthographic_view = view == ReferencePlaneView::front ||
        view == ReferencePlaneView::side || view == ReferencePlaneView::top;
    if (!safe_identifier(identity, 128U) ||
        !safe_identifier(source_asset_identity, 128U) ||
        !safe_project_locator(source_locator) || source_digest.is_zero() ||
        source_generation.value() == 0U || source_generation.exhausted() ||
        image_width_pixels == 0U || image_height_pixels == 0U ||
        image_width_pixels > kMaxReferenceImageDimension ||
        image_height_pixels > kMaxReferenceImageDimension ||
        static_cast<std::uint64_t>(image_width_pixels) * image_height_pixels >
            kMaxReferenceImagePixels ||
        !valid_view || !safe_identifier(view_set_identity, 128U, true) ||
        (orthographic_view && view_set_identity.empty()) ||
        (view == ReferencePlaneView::camera_pinned && !pinned_camera.has_value()) ||
        (view != ReferencePlaneView::camera_pinned && pinned_camera.has_value()) ||
        (camera_local_placement && view != ReferencePlaneView::camera_pinned) ||
        !within_source_bounds(origin) || !normal.finite() || !up.finite() ||
        !std::isfinite(width_meters) || width_meters <= 1.0e-6 ||
        width_meters > kMaxSourceValue || !std::isfinite(opacity) ||
        opacity < 0.0 || opacity > 1.0 || revision.value() == 0U ||
        revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "reference-image identity, source, view, placement, or display state is invalid"));
    }
    const double normal_length = normal.length();
    const double up_length = up.length();
    if (!std::isfinite(normal_length) || !std::isfinite(up_length) ||
        std::abs(normal_length - 1.0) > kReferenceBasisTolerance ||
        std::abs(up_length - 1.0) > kReferenceBasisTolerance ||
        std::abs(core::dot(normal, up)) > kReferenceBasisTolerance) {
        return core::Result<void>::failure(validation(
            "reference-image normal and up vectors must form an orthonormal plane basis"));
    }
    if (calibration.has_value()) {
        if (auto result = calibration->validate(image_width_pixels, image_height_pixels);
            !result) return result;
        const auto calibrated_width = calibration->calibrated_width_meters(
            image_width_pixels, image_height_pixels);
        if (!calibrated_width || std::abs(calibrated_width.value() - width_meters) >
                std::max(1.0, width_meters) * 1.0e-9) {
            return core::Result<void>::failure(validation(
                "reference-image plane width does not match its known-distance calibration"));
        }
    }
    return core::Result<void>::success();
}

core::Result<ReferenceImagePlaneCorners> reference_image_plane_corners(
    const ReferenceImagePlane& plane) {
    if (auto result = plane.validate(); !result) {
        return core::Result<ReferenceImagePlaneCorners>::failure(
            result.error().with_context("reference-image plane geometry"));
    }
    const core::Vec3d right = core::cross(plane.up, plane.normal).normalized();
    const double half_width = plane.width_meters * 0.5;
    const double image_height = plane.width_meters *
        static_cast<double>(plane.image_height_pixels) /
        static_cast<double>(plane.image_width_pixels);
    const double half_height = image_height * 0.5;
    if (!right.finite() || right.length() < 1.0 - kReferenceBasisTolerance ||
        !std::isfinite(half_width) || !std::isfinite(half_height)) {
        return core::Result<ReferenceImagePlaneCorners>::failure(validation(
            "reference-image plane geometry exceeds finite bounds"));
    }
    ReferenceImagePlaneCorners corners{
        plane.origin - right * half_width + plane.up * half_height,
        plane.origin + right * half_width + plane.up * half_height,
        plane.origin + right * half_width - plane.up * half_height,
        plane.origin - right * half_width - plane.up * half_height,
    };
    if (!within_source_bounds(corners.upper_left) ||
        !within_source_bounds(corners.upper_right) ||
        !within_source_bounds(corners.lower_right) ||
        !within_source_bounds(corners.lower_left)) {
        return core::Result<ReferenceImagePlaneCorners>::failure(validation(
            "reference-image plane corners exceed project placement bounds"));
    }
    return core::Result<ReferenceImagePlaneCorners>::success(corners);
}

core::Result<ReferenceImagePlanePlanCorners> reference_image_plane_xz_corners(
    const ReferenceImagePlane& plane) {
    if (auto result = plane.validate(); !result) {
        return core::Result<ReferenceImagePlanePlanCorners>::failure(
            result.error().with_context("reference-image plan projection"));
    }
    constexpr double kPlanAxisTolerance = 1.0e-6;
    if (plane.view != ReferencePlaneView::top ||
        std::abs(std::abs(plane.normal.y) - 1.0) > kPlanAxisTolerance ||
        std::abs(plane.up.y) > kPlanAxisTolerance) {
        return core::Result<ReferenceImagePlanePlanCorners>::failure(validation(
            "only X/Z-aligned top reference planes project into the plan work surface"));
    }
    const auto corners = reference_image_plane_corners(plane);
    if (!corners) {
        return core::Result<ReferenceImagePlanePlanCorners>::failure(corners.error());
    }
    const ReferenceImagePlanePlanCorners projected{
        {corners.value().upper_left.x, corners.value().upper_left.z},
        {corners.value().upper_right.x, corners.value().upper_right.z},
        {corners.value().lower_right.x, corners.value().lower_right.z},
        {corners.value().lower_left.x, corners.value().lower_left.z},
    };
    if (!projected.upper_left.finite() || !projected.upper_right.finite() ||
        !projected.lower_right.finite() || !projected.lower_left.finite()) {
        return core::Result<ReferenceImagePlanePlanCorners>::failure(validation(
            "reference-image plan projection produced non-finite corners"));
    }
    return core::Result<ReferenceImagePlanePlanCorners>::success(projected);
}

ReferenceImageSourceState inspect_reference_image_source(
    const ReferenceImagePlane& plane,
    const assets::ProjectAssetRegistry& registry,
    std::optional<assets::Sha256Digest> observed_source_digest) {
    if (!plane.validate()) return ReferenceImageSourceState::stale;
    const auto found = registry.records().find(plane.source_asset_identity);
    if (found == registry.records().end()) return ReferenceImageSourceState::missing;
    const auto& source = found->second;
    if (source.kind != assets::SourceAssetKind::image ||
        source.relative_source_path != plane.source_locator ||
        source.source_generation != plane.source_generation ||
        source.source_digest != plane.source_digest) {
        return ReferenceImageSourceState::stale;
    }
    if (!observed_source_digest.has_value()) return ReferenceImageSourceState::missing;
    if (observed_source_digest->is_zero() ||
        *observed_source_digest != plane.source_digest) {
        return ReferenceImageSourceState::stale;
    }
    return ReferenceImageSourceState::verified;
}

core::Result<std::vector<std::uint8_t>> read_reference_image_source_bytes(
    const std::filesystem::path& requested_project_root,
    const ReferenceImagePlane& plane,
    const assets::ProjectAssetRegistry& registry) {
    if (auto result = plane.validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            result.error().with_context("reference-image source binding"));
    }
    const auto source = registry.records().find(plane.source_asset_identity);
    if (source == registry.records().end()) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "reference-image source registration is missing"));
    }
    if (inspect_reference_image_source(plane, registry, source->second.source_digest) !=
        ReferenceImageSourceState::verified) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "reference-image source registration no longer matches its binding"));
    }

    assets::TextureSourceLimits limits;
    std::error_code error;
    const auto native_requested_root = core::native_io_path(requested_project_root, error);
    if (error) return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "reference-image project root is unavailable"));
    const auto project_root = std::filesystem::weakly_canonical(native_requested_root, error);
    if (error || project_root.empty()) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "reference-image project root is unavailable"));
    }
    const auto native_project_root = core::native_io_path(project_root, error);
    if (error || !std::filesystem::is_directory(native_project_root, error) || error) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "reference-image project root is unavailable"));
    }

    assets::BlobStore store(
        project_root / ".carto-assets" / "source-blobs",
        assets::BlobStoreLimits{limits.max_source_bytes});
    const auto expected_path = store.path_for(plane.source_digest);
    const auto expected_relative = expected_path.lexically_relative(project_root);
    if (expected_relative.empty() || expected_relative.is_absolute() ||
        expected_relative.generic_string() != plane.source_locator ||
        std::any_of(expected_relative.begin(), expected_relative.end(),
            [](const auto& component) { return component == ".."; })) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "reference-image locator is not its project-local content-addressed blob"));
    }
    const auto native_expected_path = core::native_io_path(expected_path, error);
    if (error) return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the reference-image source path"));
    const auto stored_path = std::filesystem::canonical(native_expected_path, error);
    if (error) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "reference-image source bytes are missing"));
    }
    const auto stored_relative = stored_path.lexically_relative(project_root);
    if (stored_path.lexically_normal() != expected_path.lexically_normal() ||
        stored_relative.empty() || stored_relative.is_absolute() ||
        stored_relative.generic_string() != plane.source_locator ||
        std::any_of(stored_relative.begin(), stored_relative.end(),
            [](const auto& component) { return component == ".."; })) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "reference-image source path resolves outside its project-local blob"));
    }
    const auto native_stored_path = core::native_io_path(stored_path, error);
    if (error) return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the reference-image source path"));
    const auto size = std::filesystem::file_size(native_stored_path, error);
    if (error || size == 0U || size > limits.max_source_bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "reference-image source is empty or exceeds the inspection limit"));
    }
    auto bytes = store.read(plane.source_digest);
    if (!bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            bytes.error().with_context("reference-image source read"));
    }
    const auto inspected = assets::inspect_texture_source(bytes.value(), {}, limits);
    if (!inspected) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            inspected.error().with_context("reference-image source inspection"));
    }
    if (inspected.value().source.digest != plane.source_digest ||
        inspected.value().width != plane.image_width_pixels ||
        inspected.value().height != plane.image_height_pixels) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "reference-image bytes do not match the bound digest or dimensions"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes.value()));
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

core::Result<void> PresentationCatalog::replace_camera(AuthoredCamera camera) {
    if (auto result = camera.validate(); !result) return result;
    const auto existing = cameras_.find(camera.object);
    if (existing == cameras_.end() || camera.identity != existing->second.identity ||
        camera.revision != existing->second.revision.next())
        return core::Result<void>::failure(invalid("camera replacement requires an existing identity and newer revision"));
    // Validate the whole candidate so active-camera uniqueness is failure-atomic.
    auto candidate = *this;
    candidate.cameras_.at(camera.object) = std::move(camera);
    if (auto result = candidate.validate(); !result) return result;
    *this = std::move(candidate);
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

core::Result<void> AuthoredEnvironment::validate() const {
    if (!safe_identifier(identity, 128) || revision == core::Revision{} || height < 2 ||
        height > 64 || width != height * 2 ||
        linear_radiance.size() != std::size_t(width) * height || !std::isfinite(rotation_radians) ||
        std::abs(rotation_radians) > 1e6 || !std::isfinite(intensity) || intensity < 0 ||
        intensity > 1e4)
        return core::Result<void>::failure(
            invalid("authored environment source exceeds its bounds"));
    for (const auto c : linear_radiance)
        if (!c.finite() || c.x < 0 || c.y < 0 || c.z < 0 || c.x > 1e6 || c.y > 1e6 || c.z > 1e6)
            return core::Result<void>::failure(
                invalid("environment radiance must be finite, nonnegative linear HDR"));
    return core::Result<void>::success();
}
core::Result<void> PresentationCatalog::set_environment(std::optional<AuthoredEnvironment> source) {
    if (source)
        if (auto valid = source->validate(); !valid)
            return valid;
    environment_ = std::move(source);
    return core::Result<void>::success();
}

core::Result<void> PresentationCatalog::insert_reference_plane(
    ReferenceImagePlane plane) {
    if (auto result = plane.validate(); !result) return result;
    if (reference_planes_.size() >= kMaxRecords) {
        return core::Result<void>::failure(validation(
            "reference-image catalog is at capacity"));
    }
    const std::string identity = plane.identity;
    const auto [iterator, inserted] = reference_planes_.emplace(identity, std::move(plane));
    static_cast<void>(iterator);
    if (!inserted) return core::Result<void>::failure(invalid(
        "reference-image plane identity is duplicated"));
    return core::Result<void>::success();
}

core::Result<void> PresentationCatalog::validate() const {
    if (environment_)
        if (auto valid = environment_->validate(); !valid)
            return valid;
    if (cameras_.size() > kMaxRecords || lights_.size() > kMaxRecords ||
        reference_planes_.size() > kMaxRecords) {
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
    std::map<std::string, unsigned> orthographic_view_sets;
    for (const auto& [identity, plane] : reference_planes_) {
        if (identity != plane.identity) return core::Result<void>::failure(validation(
            "reference-image map identity does not match its source record"));
        if (auto result = plane.validate(); !result) return result;
        if (plane.view == ReferencePlaneView::camera_pinned &&
            !cameras_.contains(*plane.pinned_camera)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "camera-pinned reference image references a missing authored camera"));
        }
        if (plane.view == ReferencePlaneView::front ||
            plane.view == ReferencePlaneView::side ||
            plane.view == ReferencePlaneView::top) {
            const unsigned bit = 1U << static_cast<unsigned>(plane.view);
            unsigned& mask = orthographic_view_sets[plane.view_set_identity];
            if ((mask & bit) != 0U) return core::Result<void>::failure(validation(
                "reference-image view set duplicates a front, side, or top plane"));
            mask |= bit;
        }
    }
    for (const auto& [identity, mask] : orthographic_view_sets) {
        static_cast<void>(identity);
        if (mask != 0b111U) return core::Result<void>::failure(validation(
            "reference-image view set requires exactly one front, side, and top plane"));
    }
    if (serialize().size() > kMaxSerializedBytes) {
        return core::Result<void>::failure(validation(
            "presentation catalog exceeds its serialized size bound"));
    }
    return core::Result<void>::success();
}

std::string PresentationCatalog::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
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
            << ' ' << std::quoted(camera.reference_image_identity) << ' '
            << camera.exposure_ev << ' ' << static_cast<unsigned>(camera.tone_mode) << '\n';
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
    out << "REFERENCE_PLANES " << reference_planes_.size() << '\n';
    for (const auto& [identity, plane] : reference_planes_) {
        static_cast<void>(identity);
        out << "REFERENCE_PLANE " << static_cast<unsigned>(plane.view) << ' '
            << plane.source_generation.value() << ' ' << plane.revision.value() << ' '
            << plane.image_width_pixels << ' ' << plane.image_height_pixels << ' '
            << plane.origin.x << ' ' << plane.origin.y << ' ' << plane.origin.z << ' '
            << plane.normal.x << ' ' << plane.normal.y << ' ' << plane.normal.z << ' '
            << plane.up.x << ' ' << plane.up.y << ' ' << plane.up.z << ' '
            << plane.width_meters << ' ' << plane.opacity << ' '
            << (plane.visible ? 1U : 0U) << ' ' << (plane.locked ? 1U : 0U) << ' '
            << (plane.camera_local_placement ? 1U : 0U) << ' '
            << (plane.pinned_camera.has_value() ? plane.pinned_camera->value : 0U) << ' '
            << (plane.calibration.has_value() ? 1U : 0U) << ' '
            << (plane.calibration.has_value() ? plane.calibration->first_pixel.x : 0.0) << ' '
            << (plane.calibration.has_value() ? plane.calibration->first_pixel.y : 0.0) << ' '
            << (plane.calibration.has_value() ? plane.calibration->second_pixel.x : 0.0) << ' '
            << (plane.calibration.has_value() ? plane.calibration->second_pixel.y : 0.0) << ' '
            << (plane.calibration.has_value() ?
                plane.calibration->known_distance_meters : 0.0) << ' '
            << plane.source_digest.hex() << ' ' << std::quoted(plane.identity) << ' '
            << std::quoted(plane.source_asset_identity) << ' '
            << std::quoted(plane.source_locator) << ' '
            << std::quoted(plane.view_set_identity) << '\n';
    }
    out << "ENVIRONMENT " << (environment_ ? 1 : 0) << '\n';
    if (environment_) {
        const auto &e = *environment_;
        out << "ENVIRONMENT_SOURCE " << std::quoted(e.identity) << ' ' << e.revision.value() << ' '
            << e.width << ' ' << e.height << ' ' << e.rotation_radians << ' ' << e.intensity << ' '
            << e.enabled << '\n';
        for (const auto c : e.linear_radiance)
            out << "RADIANCE " << c.x << ' ' << c.y << ' ' << c.z << '\n';
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
    auto parse = [&](bool main_v3) -> core::Result<PresentationCatalog> {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::uint32_t version = 0U;
    if (!expect(input, kMagic) || !read_unsigned(input, version) ||
        (version < 1U || version > kSchemaVersion)) {
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
        if (version >= 4U || (version == 3U && !main_v3)) {
            unsigned tone = 0U;
            if (!(input >> camera.exposure_ev) || !read_enum(input, 2U, tone))
                return core::Result<PresentationCatalog>::failure(validation(
                    "presentation camera display record is malformed"));
            camera.tone_mode = static_cast<render::ToneMapMode>(tone);
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
    if (version >= 2U) {
        if (!expect(input, "REFERENCE_PLANES") || !read_count(input, kMaxRecords, count)) {
            return core::Result<PresentationCatalog>::failure(validation(
                "presentation reference-image count is invalid"));
        }
        for (std::size_t index = 0U; index < count; ++index) {
            ReferenceImagePlane plane;
            unsigned view = 0U;
            std::uint64_t source_generation = 0U;
            std::uint64_t revision = 0U;
            std::uint64_t camera_object = 0U;
            bool has_calibration = false;
            bool visible = false;
            bool locked = false;
            bool camera_local_placement = false;
            double first_x = 0.0;
            double first_y = 0.0;
            double second_x = 0.0;
            double second_y = 0.0;
            double known_distance = 0.0;
            std::string encoded_digest;
            if (!expect(input, "REFERENCE_PLANE") ||
                !read_enum(input, static_cast<unsigned>(ReferencePlaneView::camera_pinned), view) ||
                !read_unsigned(input, source_generation) || !read_unsigned(input, revision) ||
                !read_unsigned(input, plane.image_width_pixels) ||
                !read_unsigned(input, plane.image_height_pixels) ||
                !(input >> plane.origin.x >> plane.origin.y >> plane.origin.z >>
                  plane.normal.x >> plane.normal.y >> plane.normal.z >>
                  plane.up.x >> plane.up.y >> plane.up.z >> plane.width_meters >>
                  plane.opacity) ||
                !read_bool(input, visible) || !read_bool(input, locked) ||
                ((version >= 5U || (version == 3U && main_v3)) &&
                 !read_bool(input, camera_local_placement)) ||
                !read_unsigned(input, camera_object) || !read_bool(input, has_calibration) ||
                !(input >> first_x >> first_y >> second_x >> second_y >> known_distance >>
                  encoded_digest >> std::quoted(plane.identity) >>
                  std::quoted(plane.source_asset_identity) >>
                  std::quoted(plane.source_locator) >> std::quoted(plane.view_set_identity))) {
                return core::Result<PresentationCatalog>::failure(validation(
                    "presentation reference-image record is malformed"));
            }
            const auto parsed_digest = assets::Sha256Digest::from_hex(encoded_digest);
            if (!parsed_digest) return core::Result<PresentationCatalog>::failure(
                parsed_digest.error().with_context("reference-image source digest"));
            plane.source_digest = parsed_digest.value();
            plane.source_generation = core::Revision{source_generation};
            plane.revision = core::Revision{revision};
            plane.view = static_cast<ReferencePlaneView>(view);
            plane.visible = visible;
            plane.locked = locked;
            plane.camera_local_placement = camera_local_placement;
            if (camera_object != 0U) plane.pinned_camera = scene::ObjectId{camera_object};
            if (has_calibration) {
                plane.calibration = ReferenceImageCalibration{
                    {first_x, first_y}, {second_x, second_y}, known_distance};
            } else if (first_x != 0.0 || first_y != 0.0 || second_x != 0.0 ||
                second_y != 0.0 || known_distance != 0.0) {
                return core::Result<PresentationCatalog>::failure(validation(
                    "reference-image plane has unbound calibration data"));
            }
            if (auto result = catalog.insert_reference_plane(std::move(plane)); !result) {
                return core::Result<PresentationCatalog>::failure(result.error());
            }
        }
    }
    if (version >= 4) {
        bool present = false;
        if (!expect(input, "ENVIRONMENT") || !read_bool(input, present))
            return core::Result<PresentationCatalog>::failure(
                validation("environment presence record is missing"));
        if (present) {
            AuthoredEnvironment e;
            std::uint64_t revision = 0;
            if (!expect(input, "ENVIRONMENT_SOURCE") || !(input >> std::quoted(e.identity)) ||
                !read_unsigned(input, revision) || !read_unsigned(input, e.width) ||
                !read_unsigned(input, e.height) || !(input >> e.rotation_radians >> e.intensity) ||
                !read_bool(input, e.enabled) || e.height < 2 || e.height > 64 ||
                e.width != 2 * e.height)
                return core::Result<PresentationCatalog>::failure(
                    validation("environment source record is invalid"));
            e.revision = core::Revision{revision};
            e.linear_radiance.resize(std::size_t(e.width) * e.height);
            for (auto &c : e.linear_radiance)
                if (!expect(input, "RADIANCE") || !(input >> c.x >> c.y >> c.z))
                    return core::Result<PresentationCatalog>::failure(
                        validation("environment pixel record is invalid"));
            if (auto added = catalog.set_environment(std::move(e)); !added)
                return core::Result<PresentationCatalog>::failure(added.error());
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
    };
    // Version 3 exists in two incompatible histories. Parse each complete
    // grammar and accept only a unique interpretation (or identical results).
    std::istringstream header{std::string(text)};
    std::uint32_t version = 0U;
    if (!expect(header, kMagic) || !read_unsigned(header, version))
        return core::Result<PresentationCatalog>::failure({
            core::ErrorCode::version_mismatch, "presentation catalog version is unsupported"});
    auto main_layout = parse(true);
    if (version != 3U) return main_layout;
    auto realism_layout = parse(false);
    if (main_layout && realism_layout) {
        if (main_layout.value().serialize() != realism_layout.value().serialize())
            return core::Result<PresentationCatalog>::failure({
                core::ErrorCode::version_mismatch, "ambiguous presentation catalog v3 layout"});
        return main_layout;
    }
    if (main_layout) return main_layout;
    if (realism_layout) return realism_layout;
    return core::Result<PresentationCatalog>::failure({
        core::ErrorCode::version_mismatch, "invalid or mixed presentation catalog v3 layout"});
}

} // namespace carto::project
