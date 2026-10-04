#include <carto/surface/process.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::surface {

namespace {

constexpr double kEpsilon = 1e-12;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool valid_digest(std::string_view value) {
    return value.size() == 64U && assets::Sha256Digest::from_hex(value).has_value();
}

std::string digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

std::string vec3_canonical(core::Vec3d value) {
    std::ostringstream output;
    output << std::setprecision(17) << value.x << ',' << value.y << ',' << value.z;
    return output.str();
}

bool valid_revision(core::Revision revision) noexcept {
    return !revision.exhausted();
}

} // namespace

core::Result<void> MaterialLayer::validate() const {
    if (material == 0U || !std::isfinite(nominal_thickness) || nominal_thickness <= 0.0 ||
        !finite_nonnegative(variation) || variation > nominal_thickness || process_class.empty() ||
        process_class.size() > 128U || process_class.find('\n') != std::string::npos) {
        return core::Result<void>::failure(invalid("material layer is invalid"));
    }
    return core::Result<void>::success();
}

std::string MaterialStratigraphy::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "material-stratigraphy-v1;domain=" << domain.value
           << ";revision=" << source_revision.value() << ";layers=";
    for (const auto& layer : layers) {
        output << layer.material << ',' << layer.nominal_thickness << ',' << layer.variation << ','
               << layer.process_class << ';';
    }
    return output.str();
}

core::Result<void> MaterialStratigraphy::validate() const {
    if (!domain || !valid_revision(source_revision) || layers.empty() || !valid_digest(digest) ||
        digest != digest_text(canonical())) {
        return core::Result<void>::failure(validation("material stratigraphy identity is invalid"));
    }
    for (const auto& layer : layers) {
        if (auto result = layer.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialStratigraphy::refresh_digest() {
    if (!domain || !valid_revision(source_revision) || layers.empty()) {
        return core::Result<void>::failure(validation(
            "material stratigraphy cannot refresh from incomplete identity"));
    }
    for (const auto& layer : layers) {
        if (auto result = layer.validate(); !result) return result;
    }
    digest = digest_text(canonical());
    return core::Result<void>::success();
}

core::Result<void> MaterialExposure::validate() const {
    if (material == 0U || !finite_nonnegative(depth_into_layer)) {
        return core::Result<void>::failure(validation("material exposure is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<MaterialExposure> resolve_material_exposure(
    const MaterialStratigraphy& stratigraphy,
    double damage_depth) {
    if (auto result = stratigraphy.validate(); !result) {
        return core::Result<MaterialExposure>::failure(result.error());
    }
    if (!finite_nonnegative(damage_depth)) {
        return core::Result<MaterialExposure>::failure(invalid("damage depth is invalid"));
    }
    double remaining = damage_depth;
    for (std::size_t index = 0U; index < stratigraphy.layers.size(); ++index) {
        const auto& layer = stratigraphy.layers[index];
        if (remaining <= layer.nominal_thickness) {
            MaterialExposure result{
                layer.material,
                static_cast<std::uint32_t>(index),
                remaining,
                false,
            };
            return core::Result<MaterialExposure>::success(std::move(result));
        }
        remaining -= layer.nominal_thickness;
    }
    const auto& substrate = stratigraphy.layers.back();
    MaterialExposure result{
        substrate.material,
        static_cast<std::uint32_t>(stratigraphy.layers.size() - 1U),
        remaining,
        true,
    };
    return core::Result<MaterialExposure>::success(std::move(result));
}

core::Result<void> WeatheringControls::validate() const {
    if (!finite_nonnegative(age) || !finite_unit(rain_exposure) ||
        !finite_unit(solar_exposure) || !finite_unit(moisture) || !finite_unit(traffic) ||
        !finite_unit(maintenance)) {
        return core::Result<void>::failure(invalid("weathering controls are invalid"));
    }
    return core::Result<void>::success();
}

std::string WeatheringControls::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << age << ',' << rain_exposure << ',' << solar_exposure << ','
           << moisture << ',' << traffic << ',' << maintenance << ',' << seed;
    return output.str();
}

std::string WeatheringState::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "weathering-state-v1;domain=" << domain.value
           << ";revision=" << source_revision.value() << ';' << runoff << ','
           << moisture_retention << ',' << solar_fade << ',' << abrasion << ',' << oxidation << ','
           << biological_growth << ',' << contact_wear;
    return output.str();
}

core::Result<void> WeatheringState::validate() const {
    if (!domain || !valid_revision(source_revision) || !finite_unit(runoff) ||
        !finite_unit(moisture_retention) || !finite_unit(solar_fade) || !finite_unit(abrasion) ||
        !finite_unit(oxidation) || !finite_unit(biological_growth) || !finite_unit(contact_wear) ||
        !valid_digest(digest) || digest != digest_text(canonical())) {
        return core::Result<void>::failure(validation("weathering state is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> WeatheringState::refresh_digest() {
    if (!domain || !valid_revision(source_revision) || !finite_unit(runoff) ||
        !finite_unit(moisture_retention) || !finite_unit(solar_fade) || !finite_unit(abrasion) ||
        !finite_unit(oxidation) || !finite_unit(biological_growth) || !finite_unit(contact_wear)) {
        return core::Result<void>::failure(validation(
            "weathering state cannot refresh from invalid values"));
    }
    digest = digest_text(canonical());
    return core::Result<void>::success();
}

core::Result<WeatheringState> derive_weathering(
    const SurfaceAnalysisRecord& analysis,
    const WeatheringControls& controls) {
    if (auto result = controls.validate(); !result) {
        return core::Result<WeatheringState>::failure(result.error());
    }
    if (!analysis.domain.id || !valid_revision(analysis.domain.source_revision) ||
        !analysis.mean_normal.finite() || !analysis.visibility.validate() ||
        !analysis.curvature.validate()) {
        return core::Result<WeatheringState>::failure(validation(
            "weathering requires a valid surface analysis identity"));
    }
    const double age_factor = std::clamp(controls.age / 100.0, 0.0, 1.0);
    const double cavity = std::clamp(analysis.visibility.cavity, 0.0, 1.0);
    const double orientation = std::clamp(std::abs(analysis.mean_normal.z), 0.0, 1.0);
    const double seed_factor = static_cast<double>(controls.seed % 997U) / 997.0;
    WeatheringState result{
        analysis.domain.id,
        analysis.domain.source_revision,
        std::clamp(controls.rain_exposure * (0.5 + 0.5 * cavity) * (1.0 - controls.maintenance), 0.0, 1.0),
        std::clamp(controls.moisture * (0.5 + 0.5 * cavity) * (0.5 + 0.5 * age_factor), 0.0, 1.0),
        std::clamp(controls.solar_exposure * orientation * age_factor, 0.0, 1.0),
        std::clamp(controls.traffic * (0.5 + 0.5 * age_factor) * (1.0 - controls.maintenance), 0.0, 1.0),
        0.0,
        0.0,
        0.0,
        {},
    };
    result.oxidation = std::clamp(result.moisture_retention * (0.5 + 0.5 * age_factor) *
        (0.9 + 0.1 * seed_factor), 0.0, 1.0);
    result.biological_growth = std::clamp(result.moisture_retention *
        (1.0 - result.solar_fade) * 0.75, 0.0, 1.0);
    result.contact_wear = std::clamp(result.abrasion * (0.5 + 0.5 * controls.traffic), 0.0, 1.0);
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<WeatheringState>::failure(refreshed.error());
    }
    return core::Result<WeatheringState>::success(std::move(result));
}

core::Result<void> ManufacturingStep::validate() const {
    if (sequence == 0U || !direction.finite() || direction.length_squared() <= kEpsilon ||
        !finite_unit(intensity) || evidence.size() > 256U || evidence.find('\n') != std::string::npos) {
        return core::Result<void>::failure(invalid("manufacturing step is invalid"));
    }
    return core::Result<void>::success();
}

std::string ManufacturingHistory::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "manufacturing-history-v1;domain=" << domain.value
           << ";revision=" << source_revision.value() << ";steps=";
    for (const auto& step : steps) {
        output << step.sequence << ',' << static_cast<int>(step.process) << ','
               << vec3_canonical(step.direction) << ',' << step.intensity << ',' << step.evidence << ';';
    }
    return output.str();
}

core::Result<void> ManufacturingHistory::validate() const {
    if (!domain || !valid_revision(source_revision) || steps.empty() ||
        !std::is_sorted(steps.begin(), steps.end(), [](const auto& left, const auto& right) {
            return left.sequence < right.sequence;
        }) || !valid_digest(digest) || digest != digest_text(canonical())) {
        return core::Result<void>::failure(validation("manufacturing history is invalid"));
    }
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        if (auto result = steps[index].validate(); !result) return result;
        if (index != 0U && steps[index - 1U].sequence == steps[index].sequence) {
            return core::Result<void>::failure(validation(
                "manufacturing history contains duplicate sequence numbers"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ManufacturingHistory::refresh_digest() {
    if (!domain || !valid_revision(source_revision) || steps.empty()) {
        return core::Result<void>::failure(validation(
            "manufacturing history cannot refresh from incomplete identity"));
    }
    for (const auto& step : steps) {
        if (auto result = step.validate(); !result) return result;
    }
    if (!std::is_sorted(steps.begin(), steps.end(), [](const auto& left, const auto& right) {
            return left.sequence < right.sequence;
        }) || std::adjacent_find(steps.begin(), steps.end(), [](const auto& left, const auto& right) {
            return left.sequence == right.sequence;
        }) != steps.end()) {
        return core::Result<void>::failure(validation(
            "manufacturing history sequence numbers are not strictly ordered"));
    }
    digest = digest_text(canonical());
    return core::Result<void>::success();
}

std::string SurfaceContact::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "surface-contact-v1;" << a.value << ',' << b.value << ','
           << static_cast<int>(type) << ',' << proximity;
    return output.str();
}

core::Result<void> SurfaceContact::validate() const {
    if (!a || !b || a == b || !(a < b) || !finite_nonnegative(proximity) ||
        !valid_digest(digest) || digest != digest_text(canonical())) {
        return core::Result<void>::failure(validation("surface contact is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceContact::refresh_digest() {
    if (!a || !b || a == b || !(a < b) || !finite_nonnegative(proximity)) {
        return core::Result<void>::failure(validation(
            "surface contact cannot refresh from invalid endpoints"));
    }
    digest = digest_text(canonical());
    return core::Result<void>::success();
}

std::string ContactGraph::canonical() const {
    std::ostringstream output;
    output << "contact-graph-v1;revision=" << source_revision.value() << '\n';
    for (const auto& contact : contacts) output << contact.canonical() << '\n';
    return output.str();
}

core::Result<void> ContactGraph::validate() const {
    if (!valid_revision(source_revision) || !valid_digest(digest) || digest != digest_text(canonical())) {
        return core::Result<void>::failure(validation("contact graph identity is invalid"));
    }
    std::set<std::pair<SurfaceDomainId, SurfaceDomainId>> unique;
    for (const auto& contact : contacts) {
        if (auto result = contact.validate(); !result) return result;
        if (!unique.emplace(contact.a, contact.b).second) {
            return core::Result<void>::failure(validation("contact graph contains a duplicate pair"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ContactGraph::refresh_digest() {
    if (!valid_revision(source_revision)) {
        return core::Result<void>::failure(validation(
            "contact graph cannot refresh from an exhausted revision"));
    }
    for (auto& contact : contacts) {
        if (auto result = contact.refresh_digest(); !result) return result;
    }
    std::sort(contacts.begin(), contacts.end(), [](const auto& left, const auto& right) {
        return std::tie(left.a, left.b) < std::tie(right.a, right.b);
    });
    if (std::adjacent_find(contacts.begin(), contacts.end(), [](const auto& left, const auto& right) {
            return left.a == right.a && left.b == right.b;
        }) != contacts.end()) {
        return core::Result<void>::failure(validation("contact graph contains a duplicate pair"));
    }
    digest = digest_text(canonical());
    return core::Result<void>::success();
}

core::Result<void> EdgeProfile::validate() const {
    if (!edge || !finite_nonnegative(nominal_radius) || !finite_nonnegative(radius_variance) ||
        !finite_unit(wear) || !finite_unit(damage) || material_transition.size() > 256U ||
        material_transition.find('\n') != std::string::npos) {
        return core::Result<void>::failure(validation("semantic edge profile is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialGeometryPolicy::validate() const {
    if (material == 0U || !std::isfinite(target_feature_scale) || target_feature_scale <= 0.0 ||
        !finite_nonnegative(max_displacement) || max_generated_faces == 0U ||
        generator_version.empty() || generator_version.size() > 128U ||
        generator_version.find('\n') != std::string::npos) {
        return core::Result<void>::failure(invalid("material geometry policy is invalid"));
    }
    return core::Result<void>::success();
}

std::string MaterialGeometryPolicy::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "material-geometry-policy-v1;material=" << material << ','
           << target_feature_scale << ',' << max_displacement << ',' << max_generated_faces << ','
           << seed << ',' << generator_version;
    return output.str();
}

assets::Sha256Digest MaterialGeometryPolicy::digest() const {
    const std::string value = canonical();
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::string MaterialGeometryIntent::canonical() const {
    std::ostringstream output;
    output << "material-geometry-intent-v1;domain=" << domain.value << ";face=" << source_face.value
           << ";revision=" << source_revision.value() << ";policy=" << policy.digest().hex();
    return output.str();
}

core::Result<void> MaterialGeometryIntent::validate() const {
    if (!domain || !source_face || !valid_revision(source_revision) ||
        !valid_digest(digest) || digest != digest_text(canonical())) {
        return core::Result<void>::failure(validation("material geometry intent is invalid"));
    }
    return policy.validate();
}

core::Result<void> MaterialGeometryIntent::refresh_digest() {
    if (!domain || !source_face || !valid_revision(source_revision)) {
        return core::Result<void>::failure(validation(
            "material geometry intent cannot refresh from incomplete identity"));
    }
    if (auto result = policy.validate(); !result) return result;
    digest = digest_text(canonical());
    return core::Result<void>::success();
}

core::Result<MaterialGeometryIntent> make_material_geometry_intent(
    SurfaceDomainId domain,
    geometry::FaceId source_face,
    core::Revision source_revision,
    const MaterialGeometryPolicy& policy) {
    MaterialGeometryIntent result{domain, source_face, source_revision, policy, {}};
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<MaterialGeometryIntent>::failure(refreshed.error());
    }
    return core::Result<MaterialGeometryIntent>::success(std::move(result));
}

} // namespace carto::surface
