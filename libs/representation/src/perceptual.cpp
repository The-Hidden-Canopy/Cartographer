#include <carto/representation/perceptual.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace carto::representation {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

double canonical_double(double value) {
    return value == 0.0 ? 0.0 : value;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool valid_purpose(RepresentationPurpose purpose) noexcept {
    switch (purpose) {
    case RepresentationPurpose::render:
    case RepresentationPurpose::collision:
    case RepresentationPurpose::navigation:
    case RepresentationPurpose::sensor:
    case RepresentationPurpose::shadow:
        return true;
    }
    return false;
}

bool valid_residual_mode(ResidualMode mode) noexcept {
    switch (mode) {
    case ResidualMode::none:
    case ResidualMode::normal:
    case ResidualMode::displacement:
    case ResidualMode::radiance:
    case ResidualMode::sparse_patch:
        return true;
    }
    return false;
}

bool valid_role(PerceptualRole role) noexcept {
    switch (role) {
    case PerceptualRole::silhouette:
    case PerceptualRole::identity:
    case PerceptualRole::contact:
    case PerceptualRole::interaction:
    case PerceptualRole::deformation:
    case PerceptualRole::shading:
    case PerceptualRole::radiance:
    case PerceptualRole::microstructure:
        return true;
    }
    return false;
}

bool valid_promotion(RepresentationPromotionState state) noexcept {
    switch (state) {
    case RepresentationPromotionState::experimental:
    case RepresentationPromotionState::certified:
    case RepresentationPromotionState::rejected:
        return true;
    }
    return false;
}

bool valid_evidence_status(EvidenceStatus status) noexcept {
    switch (status) {
    case EvidenceStatus::admitted:
    case EvidenceStatus::failed:
    case EvidenceStatus::stale:
        return true;
    }
    return false;
}

template <typename T>
void write_optional(std::ostringstream& output, const std::optional<T>& value) {
    if (value.has_value()) output << std::setprecision(17) << canonical_double(*value);
    else output << '-';
}

void write_optional_integer(
    std::ostringstream& output, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) output << *value;
    else output << '-';
}

bool error_no_greater(const RepresentationErrorEnvelope& value,
                      const RepresentationErrorEnvelope& maximum) noexcept {
    return value.max_position_error <= maximum.max_position_error &&
        value.max_silhouette_error <= maximum.max_silhouette_error &&
        value.max_normal_error <= maximum.max_normal_error &&
        value.max_radiance_error <= maximum.max_radiance_error;
}

bool source_same(const AuthoredSourceIdentity& left,
                 const AuthoredSourceIdentity& right) noexcept {
    return left == right;
}

std::uint64_t optional_cost(const std::optional<std::uint64_t>& value) noexcept {
    return value.value_or(std::numeric_limits<std::uint64_t>::max());
}

bool lower_cost(const MeasuredSurfaceCost& left,
                const MeasuredSurfaceCost& right) noexcept {
    return std::tuple{optional_cost(left.gpu_time_ns),
                      optional_cost(left.host_time_ns),
                      left.resident_bytes,
                      optional_cost(left.bandwidth_bytes)} <
        std::tuple{optional_cost(right.gpu_time_ns),
                   optional_cost(right.host_time_ns),
                   right.resident_bytes,
                   optional_cost(right.bandwidth_bytes)};
}

template <typename T>
bool optional_no_greater(const std::optional<T>& left,
                         const std::optional<T>& right) noexcept {
    if (!left.has_value()) return !right.has_value();
    if (!right.has_value()) return true;
    return *left <= *right;
}

template <typename T>
bool optional_strictly_less(const std::optional<T>& left,
                            const std::optional<T>& right) noexcept {
    if (!left.has_value()) return false;
    if (!right.has_value()) return true;
    return *left < *right;
}

bool cost_no_worse(const MeasuredSurfaceCost& left,
                   const MeasuredSurfaceCost& right) noexcept {
    return optional_no_greater(left.gpu_time_ns, right.gpu_time_ns) &&
        optional_no_greater(left.host_time_ns, right.host_time_ns) &&
        left.resident_bytes <= right.resident_bytes &&
        optional_no_greater(left.bandwidth_bytes, right.bandwidth_bytes);
}

bool cost_strictly_better(const MeasuredSurfaceCost& left,
                          const MeasuredSurfaceCost& right) noexcept {
    return optional_strictly_less(left.gpu_time_ns, right.gpu_time_ns) ||
        optional_strictly_less(left.host_time_ns, right.host_time_ns) ||
        left.resident_bytes < right.resident_bytes ||
        optional_strictly_less(left.bandwidth_bytes, right.bandwidth_bytes);
}

bool lower_cost_for_budget(const MeasuredSurfaceCost& left,
                           const MeasuredSurfaceCost& right,
                           const PerceptualBudget& budget) noexcept {
    // Prefer the axes the caller actually budgeted. The fixed fallback keeps
    // selection deterministic when the budget declares only quality bounds.
    if (budget.max_gpu_time_ns.has_value() &&
        left.gpu_time_ns != right.gpu_time_ns) {
        return *left.gpu_time_ns < *right.gpu_time_ns;
    }
    if (budget.max_host_time_ns.has_value() &&
        left.host_time_ns != right.host_time_ns) {
        return *left.host_time_ns < *right.host_time_ns;
    }
    if (budget.max_resident_bytes.has_value() &&
        left.resident_bytes != right.resident_bytes) {
        return left.resident_bytes < right.resident_bytes;
    }
    if (budget.max_bandwidth_bytes.has_value() &&
        left.bandwidth_bytes != right.bandwidth_bytes) {
        return *left.bandwidth_bytes < *right.bandwidth_bytes;
    }
    return lower_cost(left, right);
}

bool dominates(const PerceptualFrontierCandidate& left,
                const PerceptualFrontierCandidate& right) noexcept {
    if (left.purpose != right.purpose) return false;
    // Rejected evidence is never allowed to remove a usable candidate. An
    // experimental observation also cannot remove a certified one: doing so
    // would make the default certification gate depend on frontier input
    // order/content rather than on the retained evidence state.
    if (left.promotion == RepresentationPromotionState::rejected ||
        (right.promotion == RepresentationPromotionState::certified &&
         left.promotion != RepresentationPromotionState::certified)) {
        return false;
    }
    const bool no_worse =
        left.error.max_position_error <= right.error.max_position_error &&
        left.error.max_silhouette_error <= right.error.max_silhouette_error &&
        left.error.max_normal_error <= right.error.max_normal_error &&
        left.error.max_radiance_error <= right.error.max_radiance_error &&
        left.feature_recall.weighted_recall() >=
            right.feature_recall.weighted_recall() &&
        left.feature_recall.weighted_false_suppression() <=
            right.feature_recall.weighted_false_suppression() &&
        cost_no_worse(left.cost, right.cost);
    const bool strictly_better =
        left.error.max_position_error < right.error.max_position_error ||
        left.error.max_silhouette_error < right.error.max_silhouette_error ||
        left.error.max_normal_error < right.error.max_normal_error ||
        left.error.max_radiance_error < right.error.max_radiance_error ||
        left.feature_recall.weighted_recall() >
            right.feature_recall.weighted_recall() ||
        left.feature_recall.weighted_false_suppression() <
            right.feature_recall.weighted_false_suppression() ||
        cost_strictly_better(left.cost, right.cost);
    return no_worse && strictly_better;
}

} // namespace

std::string_view residual_mode_name(ResidualMode mode) noexcept {
    switch (mode) {
    case ResidualMode::none: return "none";
    case ResidualMode::normal: return "normal";
    case ResidualMode::displacement: return "displacement";
    case ResidualMode::radiance: return "radiance";
    case ResidualMode::sparse_patch: return "sparse-patch";
    }
    return "unknown";
}

core::Result<ResidualMode> residual_mode_from_name(std::string_view name) {
    for (const auto mode : {ResidualMode::none, ResidualMode::normal,
                            ResidualMode::displacement, ResidualMode::radiance,
                            ResidualMode::sparse_patch}) {
        if (residual_mode_name(mode) == name) {
            return core::Result<ResidualMode>::success(mode);
        }
    }
    return core::Result<ResidualMode>::failure(
        invalid("residual mode name is unsupported"));
}

core::Result<void> ResidualSurfaceCodecRecord::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!candidate) {
        return core::Result<void>::failure(invalid(
            "residual surface candidate identity must be non-zero"));
    }
    if (base_digest.is_zero() || base_bytes == 0U) {
        return core::Result<void>::failure(validation(
            "residual surface base must bind a non-empty digest and byte count"));
    }
    if (!valid_residual_mode(residual_mode)) {
        return core::Result<void>::failure(invalid(
            "residual surface mode is invalid"));
    }
    if (residual_mode == ResidualMode::none) {
        if (!residual_digest.is_zero() || residual_bytes != 0U) {
            return core::Result<void>::failure(validation(
                "a residual-less surface cannot carry residual payload"));
        }
    } else if (residual_digest.is_zero() || residual_bytes == 0U) {
        return core::Result<void>::failure(validation(
            "a residual surface mode must bind a non-empty residual payload"));
    }
    if (!finite_nonnegative(reconstruction_error)) {
        return core::Result<void>::failure(validation(
            "residual reconstruction error is non-finite or negative"));
    }
    return core::Result<void>::success();
}

std::string ResidualSurfaceCodecRecord::canonical() const {
    std::ostringstream output;
    output << "residual-surface-codec-v1{source=" << source.source_namespace
           << ";revision=" << source.source_revision.value()
           << ";source-digest=" << source.source_digest.hex()
           << ";candidate=" << candidate.value
           << ";base=" << base_digest.hex() << ";mode="
           << residual_mode_name(residual_mode) << ";residual="
           << residual_digest.hex() << ";base-bytes=" << base_bytes
           << ";residual-bytes=" << residual_bytes << ";error="
           << std::setprecision(17) << canonical_double(reconstruction_error) << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> ResidualSurfaceCodecRecord::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

std::string_view perceptual_role_name(PerceptualRole role) noexcept {
    switch (role) {
    case PerceptualRole::silhouette: return "silhouette";
    case PerceptualRole::identity: return "identity";
    case PerceptualRole::contact: return "contact";
    case PerceptualRole::interaction: return "interaction";
    case PerceptualRole::deformation: return "deformation";
    case PerceptualRole::shading: return "shading";
    case PerceptualRole::radiance: return "radiance";
    case PerceptualRole::microstructure: return "microstructure";
    }
    return "unknown";
}

core::Result<PerceptualRole> perceptual_role_from_name(std::string_view name) {
    for (const auto role : {PerceptualRole::silhouette, PerceptualRole::identity,
                            PerceptualRole::contact, PerceptualRole::interaction,
                            PerceptualRole::deformation, PerceptualRole::shading,
                            PerceptualRole::radiance, PerceptualRole::microstructure}) {
        if (perceptual_role_name(role) == name) {
            return core::Result<PerceptualRole>::success(role);
        }
    }
    return core::Result<PerceptualRole>::failure(
        invalid("perceptual role name is unsupported"));
}

core::Result<void> PerceptualRoleWeight::validate() const {
    if (region_id == 0U) {
        return core::Result<void>::failure(invalid(
            "perceptual role region identity must be non-zero"));
    }
    if (!valid_role(role)) {
        return core::Result<void>::failure(invalid("perceptual role is invalid"));
    }
    if (!finite_unit(importance)) {
        return core::Result<void>::failure(validation(
            "perceptual role importance must be a finite value in [0,1]"));
    }
    if (protected_invariant && importance == 0.0) {
        return core::Result<void>::failure(validation(
            "a protected perceptual role must carry non-zero importance"));
    }
    return core::Result<void>::success();
}

core::Result<void> PerceptualRoleMap::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (regions.empty() || regions.size() > kMaxRegions) {
        return core::Result<void>::failure(validation(
            "perceptual role map must contain a bounded non-empty region set"));
    }
    bool has_weight = false;
    for (std::size_t index = 0U; index < regions.size(); ++index) {
        if (auto result = regions[index].validate(); !result) return result;
        has_weight = has_weight || regions[index].importance > 0.0;
        if (index > 0U) {
            const auto previous_key = std::pair{
                regions[index - 1U].region_id,
                static_cast<std::uint8_t>(regions[index - 1U].role)};
            const auto current_key = std::pair{
                regions[index].region_id,
                static_cast<std::uint8_t>(regions[index].role)};
            if (previous_key >= current_key) {
                return core::Result<void>::failure(validation(
                    "perceptual role regions must be sorted and unique"));
            }
        }
    }
    if (!has_weight) {
        return core::Result<void>::failure(validation(
            "perceptual role map must assign positive importance to at least one region"));
    }
    return core::Result<void>::success();
}

std::string PerceptualRoleMap::canonical() const {
    std::ostringstream output;
    output << "perceptual-role-map-v1{source=" << source.source_namespace
           << ";revision=" << source.source_revision.value()
           << ";source-digest=" << source.source_digest.hex() << ";regions=[";
    for (std::size_t index = 0U; index < regions.size(); ++index) {
        if (index > 0U) output << ',';
        const auto& region = regions[index];
        output << region.region_id << ':' << perceptual_role_name(region.role) << ':'
               << std::setprecision(17) << canonical_double(region.importance) << ':'
               << (region.protected_invariant ? '1' : '0');
    }
    output << "]}";
    return output.str();
}

core::Result<assets::Sha256Digest> PerceptualRoleMap::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> VisualFeatureRecall::validate() const {
    if (feature_id == 0U) {
        return core::Result<void>::failure(invalid(
            "visual feature identity must be non-zero"));
    }
    if (!valid_role(role)) {
        return core::Result<void>::failure(invalid("visual feature role is invalid"));
    }
    if (!finite_unit(importance) || !finite_unit(recall) ||
        !finite_unit(false_suppression)) {
        return core::Result<void>::failure(validation(
            "visual feature importance, recall, and suppression must be in [0,1]"));
    }
    return core::Result<void>::success();
}

core::Result<void> VisualFeatureRecallMetrics::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!candidate) {
        return core::Result<void>::failure(invalid(
            "visual feature metrics candidate identity must be non-zero"));
    }
    if (observation_digest.is_zero() || observation_count == 0U) {
        return core::Result<void>::failure(validation(
            "visual feature metrics must bind non-empty observation evidence"));
    }
    if (features.empty() || features.size() > kMaxFeatures) {
        return core::Result<void>::failure(validation(
            "visual feature metrics must contain a bounded non-empty feature set"));
    }
    bool has_weight = false;
    for (std::size_t index = 0U; index < features.size(); ++index) {
        if (auto result = features[index].validate(); !result) return result;
        has_weight = has_weight || features[index].importance > 0.0;
        if (index > 0U && features[index - 1U].feature_id >= features[index].feature_id) {
            return core::Result<void>::failure(validation(
                "visual features must be sorted by unique feature identity"));
        }
    }
    if (!has_weight) {
        return core::Result<void>::failure(validation(
            "visual feature metrics must assign positive importance to at least one feature"));
    }
    return core::Result<void>::success();
}

double VisualFeatureRecallMetrics::weighted_recall() const noexcept {
    double weighted = 0.0;
    double total = 0.0;
    for (const auto& feature : features) {
        weighted += feature.importance * feature.recall;
        total += feature.importance;
    }
    return total == 0.0 ? 0.0 : weighted / total;
}

double VisualFeatureRecallMetrics::weighted_false_suppression() const noexcept {
    double weighted = 0.0;
    double total = 0.0;
    for (const auto& feature : features) {
        weighted += feature.importance * feature.false_suppression;
        total += feature.importance;
    }
    return total == 0.0 ? 1.0 : weighted / total;
}

std::string VisualFeatureRecallMetrics::canonical() const {
    std::ostringstream output;
    output << "visual-feature-recall-v1{source=" << source.source_namespace
           << ";revision=" << source.source_revision.value()
           << ";source-digest=" << source.source_digest.hex()
           << ";candidate=" << candidate.value << ";observation="
           << observation_digest.hex() << ";count=" << observation_count << ";features=[";
    for (std::size_t index = 0U; index < features.size(); ++index) {
        if (index > 0U) output << ',';
        const auto& feature = features[index];
        output << feature.feature_id << ':' << perceptual_role_name(feature.role) << ':'
               << std::setprecision(17) << canonical_double(feature.importance) << ':'
               << canonical_double(feature.recall) << ':'
               << canonical_double(feature.false_suppression);
    }
    output << "]}";
    return output.str();
}

core::Result<assets::Sha256Digest> VisualFeatureRecallMetrics::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> MeasuredSurfaceCost::validate() const {
    if (!candidate) {
        return core::Result<void>::failure(invalid(
            "measured surface cost candidate identity must be non-zero"));
    }
    if (!safe_text(source_namespace, RepresentationFamily::kMaxNamespaceBytes)) {
        return core::Result<void>::failure(invalid(
            "measured surface cost source namespace is empty, unsafe, or too long"));
    }
    if (source_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "measured surface cost must bind the authored source digest"));
    }
    if (!safe_text(hardware_profile, kMaxHardwareProfileBytes)) {
        return core::Result<void>::failure(invalid(
            "measured surface cost hardware profile is empty, unsafe, or too long"));
    }
    if (profile_digest.is_zero() || source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "measured surface cost must bind a live profile and source revision"));
    }
    if (!valid_evidence_status(status) || resident_bytes == 0U) {
        return core::Result<void>::failure(validation(
            "measured surface cost status or resident byte observation is invalid"));
    }
    if (gpu_time_ns.has_value() && *gpu_time_ns == 0U) {
        return core::Result<void>::failure(validation(
            "measured GPU time cannot use zero as an availability sentinel"));
    }
    if (host_time_ns.has_value() && *host_time_ns == 0U) {
        return core::Result<void>::failure(validation(
            "measured host time cannot use zero as an availability sentinel"));
    }
    if (bandwidth_bytes.has_value() && *bandwidth_bytes == 0U) {
        return core::Result<void>::failure(validation(
            "measured bandwidth cannot use zero as an availability sentinel"));
    }
    if (overdraw.has_value() && !finite_nonnegative(*overdraw)) {
        return core::Result<void>::failure(validation(
            "measured overdraw is non-finite or negative"));
    }
    if (status == EvidenceStatus::admitted && !gpu_time_ns.has_value() &&
        !host_time_ns.has_value()) {
        return core::Result<void>::failure(validation(
            "admitted cost evidence must contain a measured device or host time"));
    }
    return core::Result<void>::success();
}

std::string MeasuredSurfaceCost::canonical() const {
    std::ostringstream output;
    output << "measured-surface-cost-v1{candidate=" << candidate.value
           << ";source=" << source_namespace << ";source-digest="
           << source_digest.hex() << ";profile=" << hardware_profile << ";profile-digest="
           << profile_digest.hex() << ";source-revision=" << source_revision.value()
           << ";status=" << evidence_status_name(status) << ";gpu-ns=";
    write_optional_integer(output, gpu_time_ns);
    output << ";host-ns=";
    write_optional_integer(output, host_time_ns);
    output << ";resident-bytes=" << resident_bytes << ";bandwidth-bytes=";
    write_optional_integer(output, bandwidth_bytes);
    output << ";overdraw=";
    write_optional(output, overdraw);
    output << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> MeasuredSurfaceCost::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

std::string_view promotion_state_name(RepresentationPromotionState state) noexcept {
    switch (state) {
    case RepresentationPromotionState::experimental: return "experimental";
    case RepresentationPromotionState::certified: return "certified";
    case RepresentationPromotionState::rejected: return "rejected";
    }
    return "unknown";
}

core::Result<RepresentationPromotionState> promotion_state_from_name(
    std::string_view name) {
    for (const auto state : {RepresentationPromotionState::experimental,
                             RepresentationPromotionState::certified,
                             RepresentationPromotionState::rejected}) {
        if (promotion_state_name(state) == name) {
            return core::Result<RepresentationPromotionState>::success(state);
        }
    }
    return core::Result<RepresentationPromotionState>::failure(
        invalid("representation promotion state name is unsupported"));
}

core::Result<void> PerceptualFrontierCandidate::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!candidate || !valid_purpose(purpose)) {
        return core::Result<void>::failure(invalid(
            "perceptual frontier candidate identity or purpose is invalid"));
    }
    if (auto result = error.validate(); !result) return result;
    if (auto result = feature_recall.validate(); !result) return result;
    if (!source_same(feature_recall.source, source) ||
        feature_recall.candidate != candidate) {
        return core::Result<void>::failure(validation(
            "perceptual feature evidence is not anchored to the candidate source"));
    }
    if (auto result = cost.validate(); !result) return result;
    if (cost.candidate != candidate || cost.source_namespace != source.source_namespace ||
        cost.source_digest != source.source_digest ||
        cost.source_revision != source.source_revision) {
        return core::Result<void>::failure(validation(
            "perceptual cost evidence is not anchored to the candidate source"));
    }
    if (cost.status != EvidenceStatus::admitted) {
        return core::Result<void>::failure(validation(
            "frontier candidates require admitted measured cost evidence"));
    }
    if (!valid_promotion(promotion)) {
        return core::Result<void>::failure(invalid(
            "perceptual frontier promotion state is invalid"));
    }
    if (promotion == RepresentationPromotionState::certified &&
        promotion_evidence_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "certified perceptual candidates must bind promotion evidence"));
    }
    return core::Result<void>::success();
}

double PerceptualFrontierCandidate::quality_score() const noexcept {
    return feature_recall.weighted_recall();
}

std::string PerceptualFrontierCandidate::canonical() const {
    std::ostringstream output;
    output << "frontier-candidate-v1{source=" << source.source_namespace
           << ";revision=" << source.source_revision.value()
           << ";source-digest=" << source.source_digest.hex()
           << ";candidate=" << candidate.value << ";purpose="
           << representation_purpose_name(purpose) << ";error=" << error.canonical()
           << ";features=" << feature_recall.canonical() << ";cost="
           << cost.canonical() << ";promotion=" << promotion_state_name(promotion)
           << ";promotion-evidence=" << promotion_evidence_digest.hex()
           << '}';
    return output.str();
}

core::Result<RepresentationFrontier> RepresentationFrontier::build(
    std::span<const PerceptualFrontierCandidate> candidates) {
    if (candidates.empty() || candidates.size() > kMaxCandidates) {
        return core::Result<RepresentationFrontier>::failure(validation(
            "representation frontier candidate count is empty or exceeds its bound"));
    }
    RepresentationFrontier frontier;
    frontier.source_ = candidates.front().source;
    frontier.hardware_profile_ = candidates.front().cost.hardware_profile;
    frontier.profile_digest_ = candidates.front().cost.profile_digest;
    for (const auto& candidate : candidates) {
        if (auto result = candidate.validate(); !result) {
            return core::Result<RepresentationFrontier>::failure(result.error());
        }
        if (!source_same(candidate.source, frontier.source_) ||
            candidate.cost.hardware_profile != frontier.hardware_profile_ ||
            candidate.cost.profile_digest != frontier.profile_digest_) {
            return core::Result<RepresentationFrontier>::failure(validation(
                "frontier candidates must share one source and measured profile"));
        }
        if (std::any_of(frontier.candidates_.begin(), frontier.candidates_.end(),
                        [&](const auto& existing) {
                            return existing.candidate == candidate.candidate;
                        })) {
            return core::Result<RepresentationFrontier>::failure(validation(
                "representation frontier candidate identities must be unique"));
        }
    }
    for (const auto& candidate : candidates) {
        const bool dominated = std::any_of(
            candidates.begin(), candidates.end(), [&](const auto& other) {
                return other.candidate != candidate.candidate &&
                    dominates(other, candidate);
            });
        if (!dominated) frontier.candidates_.push_back(candidate);
    }
    std::sort(frontier.candidates_.begin(), frontier.candidates_.end(),
              [](const auto& left, const auto& right) {
                  return left.candidate < right.candidate;
              });
    if (auto result = frontier.validate(); !result) {
        return core::Result<RepresentationFrontier>::failure(result.error());
    }
    return core::Result<RepresentationFrontier>::success(std::move(frontier));
}

core::Result<void> RepresentationFrontier::validate() const {
    if (auto result = source_.validate(); !result) return result;
    if (!safe_text(hardware_profile_, MeasuredSurfaceCost::kMaxHardwareProfileBytes) ||
        profile_digest_.is_zero()) {
        return core::Result<void>::failure(validation(
            "representation frontier must bind a measured hardware profile"));
    }
    if (candidates_.empty() || candidates_.size() > kMaxCandidates) {
        return core::Result<void>::failure(validation(
            "representation frontier candidate count is empty or exceeds its bound"));
    }
    for (std::size_t index = 0U; index < candidates_.size(); ++index) {
        if (auto result = candidates_[index].validate(); !result) return result;
        if (!source_same(candidates_[index].source, source_) ||
            candidates_[index].cost.hardware_profile != hardware_profile_ ||
            candidates_[index].cost.profile_digest != profile_digest_) {
            return core::Result<void>::failure(validation(
                "frontier candidate source or measured profile differs from the frontier"));
        }
        if (index > 0U && candidates_[index - 1U].candidate >= candidates_[index].candidate) {
            return core::Result<void>::failure(validation(
                "frontier candidates must be sorted by unique identity"));
        }
    }
    for (std::size_t left = 0U; left < candidates_.size(); ++left) {
        for (std::size_t right = 0U; right < candidates_.size(); ++right) {
            if (left != right && dominates(candidates_[left], candidates_[right])) {
                return core::Result<void>::failure(validation(
                    "representation frontier contains a dominated candidate"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string RepresentationFrontier::canonical() const {
    std::ostringstream output;
    output << "representation-frontier-v1{source=" << source_.source_namespace
           << ";revision=" << source_.source_revision.value()
           << ";source-digest=" << source_.source_digest.hex()
           << ";profile=" << hardware_profile_ << ";profile-digest="
           << profile_digest_.hex() << ";candidates=[";
    for (std::size_t index = 0U; index < candidates_.size(); ++index) {
        if (index > 0U) output << ',';
        output << candidates_[index].canonical();
    }
    output << "]}";
    return output.str();
}

core::Result<assets::Sha256Digest> RepresentationFrontier::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> PerceptualBudget::validate() const {
    if (!valid_purpose(purpose)) {
        return core::Result<void>::failure(invalid("perceptual budget purpose is invalid"));
    }
    if (auto result = max_error.validate(); !result) return result;
    if (!finite_unit(min_feature_recall) || !finite_unit(max_false_suppression)) {
        return core::Result<void>::failure(validation(
            "perceptual budget recall bounds must be finite values in [0,1]"));
    }
    return core::Result<void>::success();
}

std::string PerceptualBudget::canonical() const {
    std::ostringstream output;
    output << "perceptual-budget-v1{purpose=" << representation_purpose_name(purpose)
           << ";error=" << max_error.canonical() << ";min-recall="
           << std::setprecision(17) << canonical_double(min_feature_recall)
           << ";max-suppression=" << canonical_double(max_false_suppression)
           << ";gpu-ns=";
    write_optional_integer(output, max_gpu_time_ns);
    output << ";host-ns=";
    write_optional_integer(output, max_host_time_ns);
    output << ";resident-bytes=";
    write_optional_integer(output, max_resident_bytes);
    output << ";bandwidth-bytes=";
    write_optional_integer(output, max_bandwidth_bytes);
    output << ";certified=" << (require_certified ? '1' : '0') << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> PerceptualBudget::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> MeasuredCostDecision::validate() const {
    if (!selected_candidate || source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "measured cost decision candidate or source revision is invalid"));
    }
    if (!safe_text(hardware_profile, MeasuredSurfaceCost::kMaxHardwareProfileBytes)) {
        return core::Result<void>::failure(invalid(
            "measured cost decision hardware profile is empty, unsafe, or too long"));
    }
    if (source_digest.is_zero() || profile_digest.is_zero() || frontier_digest.is_zero() ||
        budget_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "measured cost decision must bind source, profile, frontier, and budget digests"));
    }
    if (!safe_text(algorithm, 128U)) {
        return core::Result<void>::failure(invalid(
            "measured cost decision algorithm is empty, unsafe, or too long"));
    }
    if (decision_digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "measured cost decision digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string MeasuredCostDecision::canonical_without_digest() const {
    std::ostringstream output;
    output << "measured-cost-decision-v1{candidate=" << selected_candidate.value
           << ";source-revision=" << source_revision.value() << ";source-digest="
           << source_digest.hex() << ";profile="
           << hardware_profile << ";profile-digest=" << profile_digest.hex()
           << ";frontier=" << frontier_digest.hex() << ";budget="
           << budget_digest.hex() << ";algorithm=" << algorithm << '}';
    return output.str();
}

core::Result<void> MeasuredCostDecision::refresh_digest() {
    if (!selected_candidate || source_revision.exhausted() ||
        source_digest.is_zero() ||
        !safe_text(hardware_profile, MeasuredSurfaceCost::kMaxHardwareProfileBytes) ||
        profile_digest.is_zero() || frontier_digest.is_zero() || budget_digest.is_zero() ||
        !safe_text(algorithm, 128U)) {
        return core::Result<void>::failure(validation(
            "measured cost decision cannot refresh an incomplete source or input digest"));
    }
    decision_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<MeasuredCostDecision> MeasuredCostGovernor::select(
    const RepresentationFrontier& frontier,
    const PerceptualBudget& budget) {
    if (auto result = frontier.validate(); !result) {
        return core::Result<MeasuredCostDecision>::failure(result.error());
    }
    if (auto result = budget.validate(); !result) {
        return core::Result<MeasuredCostDecision>::failure(result.error());
    }
    const auto budget_digest = budget.canonical_digest();
    const auto frontier_digest = frontier.canonical_digest();
    if (!budget_digest || !frontier_digest) {
        return core::Result<MeasuredCostDecision>::failure(validation(
            "measured cost governor could not bind its input digests"));
    }

    const PerceptualFrontierCandidate* best = nullptr;
    for (const auto& candidate : frontier.candidates()) {
        if (candidate.promotion == RepresentationPromotionState::rejected ||
            candidate.purpose != budget.purpose ||
            !error_no_greater(candidate.error, budget.max_error) ||
            candidate.feature_recall.weighted_recall() < budget.min_feature_recall ||
            candidate.feature_recall.weighted_false_suppression() >
                budget.max_false_suppression ||
            (budget.require_certified &&
             candidate.promotion != RepresentationPromotionState::certified)) {
            continue;
        }
        const auto& cost = candidate.cost;
        if ((budget.max_gpu_time_ns.has_value() &&
             (!cost.gpu_time_ns.has_value() ||
              *cost.gpu_time_ns > *budget.max_gpu_time_ns)) ||
            (budget.max_host_time_ns.has_value() &&
             (!cost.host_time_ns.has_value() ||
              *cost.host_time_ns > *budget.max_host_time_ns)) ||
            (budget.max_resident_bytes.has_value() &&
             cost.resident_bytes > *budget.max_resident_bytes) ||
            (budget.max_bandwidth_bytes.has_value() &&
             (!cost.bandwidth_bytes.has_value() ||
              *cost.bandwidth_bytes > *budget.max_bandwidth_bytes))) {
            continue;
        }
        if (best == nullptr || lower_cost_for_budget(cost, best->cost, budget) ||
            (!lower_cost_for_budget(best->cost, cost, budget) &&
             (candidate.quality_score() > best->quality_score() ||
              (candidate.quality_score() == best->quality_score() &&
               candidate.candidate < best->candidate)))) {
            best = &candidate;
        }
    }
    if (best == nullptr) {
        return core::Result<MeasuredCostDecision>::failure(validation(
            "no measured representation satisfies the perceptual budget"));
    }
    MeasuredCostDecision decision;
    decision.selected_candidate = best->candidate;
    decision.source_revision = frontier.source().source_revision;
    decision.source_digest = frontier.source().source_digest;
    decision.hardware_profile = std::string(frontier.hardware_profile());
    decision.profile_digest = frontier.profile_digest();
    decision.frontier_digest = frontier_digest.value();
    decision.budget_digest = budget_digest.value();
    if (auto result = decision.refresh_digest(); !result) {
        return core::Result<MeasuredCostDecision>::failure(result.error());
    }
    return core::Result<MeasuredCostDecision>::success(std::move(decision));
}

} // namespace carto::representation
