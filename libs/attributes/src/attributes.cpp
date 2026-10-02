#include <carto/attributes/domain.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/transfer.hpp>

#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace carto::attributes {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Result<void> validation(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

core::Result<AttributeValue> value_invalid(std::string message) {
    return core::Result<AttributeValue>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Result<AttributeValue> value_validation(std::string message) {
    return core::Result<AttributeValue>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

bool finite_value(const AttributeValue& value) {
    return std::visit([](const auto& candidate) {
        using T = std::decay_t<decltype(candidate)>;
        if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            return std::isfinite(candidate);
        } else if constexpr (std::is_same_v<T, core::Vec2d> ||
                             std::is_same_v<T, core::Vec3d> ||
                             std::is_same_v<T, core::Vec4d>) {
            return candidate.finite();
        } else {
            return true;
        }
    }, value);
}

bool type_matches(AttributeType type, const AttributeValue& value) {
    switch (type) {
    case AttributeType::boolean:
        return std::holds_alternative<bool>(value);
    case AttributeType::int32:
        return std::holds_alternative<std::int32_t>(value);
    case AttributeType::uint32:
        return std::holds_alternative<std::uint32_t>(value);
    case AttributeType::float32:
        return std::holds_alternative<float>(value);
    case AttributeType::float64:
        return std::holds_alternative<double>(value);
    case AttributeType::vec2:
        return std::holds_alternative<core::Vec2d>(value);
    case AttributeType::vec3:
        return std::holds_alternative<core::Vec3d>(value);
    case AttributeType::vec4:
    case AttributeType::color4:
        return std::holds_alternative<core::Vec4d>(value);
    case AttributeType::string_id:
    case AttributeType::object_id:
    case AttributeType::vertex_id:
        return std::holds_alternative<std::uint64_t>(value);
    }
    return false;
}

bool valid_transfer_mode_impl(TransferMode mode) noexcept {
    switch (mode) {
    case TransferMode::preserve:
    case TransferMode::interpolate:
    case TransferMode::duplicate:
    case TransferMode::invalidate:
        return true;
    }
    return false;
}

template <typename T>
T average(const std::vector<AttributeValue>& values) {
    T result{};
    for (const auto& value : values) result = result + std::get<T>(value);
    return result * (1.0 / static_cast<double>(values.size()));
}

template <>
core::Vec2d average<core::Vec2d>(const std::vector<AttributeValue>& values) {
    core::Vec2d result{};
    for (const auto& value : values) {
        const auto& candidate = std::get<core::Vec2d>(value);
        result.x += candidate.x;
        result.y += candidate.y;
    }
    const double divisor = static_cast<double>(values.size());
    return {result.x / divisor, result.y / divisor};
}

template <>
core::Vec4d average<core::Vec4d>(const std::vector<AttributeValue>& values) {
    core::Vec4d result{};
    for (const auto& value : values) {
        const auto& candidate = std::get<core::Vec4d>(value);
        result.x += candidate.x;
        result.y += candidate.y;
        result.z += candidate.z;
        result.w += candidate.w;
    }
    const double divisor = static_cast<double>(values.size());
    return {result.x / divisor, result.y / divisor, result.z / divisor, result.w / divisor};
}

template <>
float average<float>(const std::vector<AttributeValue>& values) {
    float result = 0.0F;
    for (const auto& value : values) result += std::get<float>(value);
    return result / static_cast<float>(values.size());
}

template <>
double average<double>(const std::vector<AttributeValue>& values) {
    double result = 0.0;
    for (const auto& value : values) result += std::get<double>(value);
    return result / static_cast<double>(values.size());
}

core::Result<AttributeValue> interpolated(
    AttributeType type,
    const std::vector<AttributeValue>& values) {
    if (values.empty()) return value_invalid("attribute interpolation requires a source value");
    switch (type) {
    case AttributeType::float32:
        return core::Result<AttributeValue>::success(average<float>(values));
    case AttributeType::float64:
        return core::Result<AttributeValue>::success(average<double>(values));
    case AttributeType::vec2:
        return core::Result<AttributeValue>::success(average<core::Vec2d>(values));
    case AttributeType::vec3:
        return core::Result<AttributeValue>::success(average<core::Vec3d>(values));
    case AttributeType::vec4:
    case AttributeType::color4:
        return core::Result<AttributeValue>::success(average<core::Vec4d>(values));
    default:
        return core::Result<AttributeValue>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "discrete attribute types do not support interpolation"));
    }
}

core::Result<AttributeValue> transfer_value(
    const AttributeLayer& layer,
    const std::vector<std::size_t>& sources,
    TransferMode mode) {
    if (mode == TransferMode::invalidate) return core::Result<AttributeValue>::success(
        default_value(layer.descriptor.type));
    if (sources.empty()) return value_invalid("attribute transfer requires a source index");
    if (mode == TransferMode::preserve && sources.size() != 1U) {
        return value_validation("preserve transfer requires exactly one source element");
    }
    if (mode == TransferMode::preserve || mode == TransferMode::duplicate) {
        return core::Result<AttributeValue>::success(layer.values.at(sources.front()));
    }
    return interpolated(layer.descriptor.type, [&]() {
        std::vector<AttributeValue> values;
        values.reserve(sources.size());
        for (const auto source : sources) values.push_back(layer.values.at(source));
        return values;
    }());
}

} // namespace

bool valid_transfer_mode(TransferMode mode) noexcept {
    return valid_transfer_mode_impl(mode);
}

const char* domain_name(AttributeDomain domain) noexcept {
    switch (domain) {
    case AttributeDomain::object: return "object";
    case AttributeDomain::vertex: return "vertex";
    case AttributeDomain::edge: return "edge";
    case AttributeDomain::face: return "face";
    case AttributeDomain::corner: return "corner";
    case AttributeDomain::curve_point: return "curve_point";
    case AttributeDomain::spline: return "spline";
    case AttributeDomain::joint: return "joint";
    case AttributeDomain::control_point: return "control_point";
    case AttributeDomain::brep_face: return "brep_face";
    case AttributeDomain::brep_edge: return "brep_edge";
    }
    return "unknown";
}

bool valid_domain(AttributeDomain domain) noexcept {
    return domain_name(domain) != std::string_view{"unknown"};
}

bool valid_type(AttributeType type) noexcept {
    switch (type) {
    case AttributeType::boolean:
    case AttributeType::int32:
    case AttributeType::uint32:
    case AttributeType::float32:
    case AttributeType::float64:
    case AttributeType::vec2:
    case AttributeType::vec3:
    case AttributeType::vec4:
    case AttributeType::color4:
    case AttributeType::string_id:
    case AttributeType::object_id:
    case AttributeType::vertex_id:
        return true;
    }
    return false;
}

AttributeValue default_value(AttributeType type) {
    switch (type) {
    case AttributeType::boolean: return false;
    case AttributeType::int32: return std::int32_t{0};
    case AttributeType::uint32: return std::uint32_t{0};
    case AttributeType::float32: return 0.0F;
    case AttributeType::float64: return 0.0;
    case AttributeType::vec2: return core::Vec2d{};
    case AttributeType::vec3: return core::Vec3d{};
    case AttributeType::vec4:
    case AttributeType::color4: return core::Vec4d{};
    case AttributeType::string_id:
    case AttributeType::object_id:
    case AttributeType::vertex_id: return std::uint64_t{0};
    }
    return false;
}

core::Result<void> validate_value(AttributeType type, const AttributeValue& value) {
    if (!valid_type(type) || !type_matches(type, value)) {
        return invalid("attribute value type does not match its layer type");
    }
    if (!finite_value(value)) return invalid("attribute values must be finite");
    return core::Result<void>::success();
}

core::Result<void> AttributeLayerDescriptor::validate() const {
    if (id.empty()) return invalid("attribute layer requires a stable id");
    if (!valid_domain(domain) || !valid_type(type)) {
        return invalid("attribute layer descriptor contains an unknown domain or type");
    }
    return core::Result<void>::success();
}

core::Result<void> AttributeLayer::validate(std::size_t element_count) const {
    if (auto result = descriptor.validate(); !result) return result;
    if (values.size() != element_count) {
        return validation("attribute layer value count does not match its domain count");
    }
    for (const auto& value : values) {
        if (auto result = validate_value(descriptor.type, value); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> AttributeSet::set_domain_count(
    AttributeDomain domain,
    std::size_t count) {
    if (!valid_domain(domain)) return invalid("attribute domain is invalid");
    for (const auto& [ignored, layer] : layers_) {
        static_cast<void>(ignored);
        if (layer.descriptor.domain == domain && layer.values.size() != count) {
            return validation("cannot change a domain count below an existing layer");
        }
    }
    domain_counts_[domain] = count;
    return core::Result<void>::success();
}

core::Result<void> AttributeSet::add_layer(AttributeLayer layer) {
    const auto count = domain_count(layer.descriptor.domain);
    if (!count.has_value()) {
        return invalid("attribute layer domain count must be configured before adding a layer");
    }
    if (auto result = layer.validate(*count); !result) return result;
    if (layers_.contains(layer.descriptor.id)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "attribute layer id already exists"));
    }
    layers_.emplace(layer.descriptor.id, std::move(layer));
    return core::Result<void>::success();
}

core::Result<void> AttributeSet::remove_layer(std::string_view id) {
    const auto iterator = layers_.find(std::string{id});
    if (iterator == layers_.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "attribute layer does not exist"));
    }
    layers_.erase(iterator);
    return core::Result<void>::success();
}

core::Result<void> AttributeSet::validate() const {
    for (const auto& [domain, ignored] : domain_counts_) {
        static_cast<void>(ignored);
        if (!valid_domain(domain)) return invalid("attribute set contains an invalid domain");
    }
    for (const auto& [id, layer] : layers_) {
        if (id != layer.descriptor.id) return validation("attribute layer map key does not match id");
        const auto count = domain_count(layer.descriptor.domain);
        if (!count.has_value()) return validation("attribute layer domain count is missing");
        if (auto result = layer.validate(*count); !result) return result;
    }
    return core::Result<void>::success();
}

const AttributeLayer* AttributeSet::find(std::string_view id) const noexcept {
    const auto iterator = layers_.find(std::string{id});
    return iterator == layers_.end() ? nullptr : &iterator->second;
}

std::optional<std::size_t> AttributeSet::domain_count(AttributeDomain domain) const noexcept {
    const auto iterator = domain_counts_.find(domain);
    if (iterator == domain_counts_.end()) return std::nullopt;
    return iterator->second;
}

TransferMode AttributeTransferPolicy::mode_for(std::string_view layer_id) const noexcept {
    const auto iterator = per_layer.find(std::string{layer_id});
    return iterator == per_layer.end() ? default_mode : iterator->second;
}

core::Result<void> AttributeTransferPolicy::validate(const AttributeSet& source) const {
    if (!valid_transfer_mode_impl(default_mode)) {
        return invalid("attribute transfer policy contains an unknown default mode");
    }
    for (const auto& [layer_id, mode] : per_layer) {
        if (layer_id.empty() || source.find(layer_id) == nullptr) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "attribute transfer policy references a missing layer"));
        }
        if (!valid_transfer_mode_impl(mode)) {
            return invalid("attribute transfer policy contains an unknown layer mode");
        }
    }
    return core::Result<void>::success();
}

core::Result<void> TopologyProvenance::validate(const AttributeSet& source) const {
    for (const auto& [domain, mappings] : source_indices) {
        const auto count = source.domain_count(domain);
        if (!count.has_value()) return invalid("topology provenance references an unknown domain");
        for (const auto& sources : mappings) {
            for (const auto index : sources) {
                if (index >= *count) return invalid("topology provenance index is out of bounds");
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<AttributeSet> transfer(
    const AttributeSet& source,
    TopologyProvenance provenance,
    const AttributeTransferPolicy& policy) {
    if (auto result = source.validate(); !result) {
        return core::Result<AttributeSet>::failure(result.error());
    }
    if (auto result = provenance.validate(source); !result) {
        return core::Result<AttributeSet>::failure(result.error());
    }
    if (auto result = policy.validate(source); !result) {
        return core::Result<AttributeSet>::failure(result.error());
    }
    std::map<AttributeDomain, std::size_t> new_domain_counts;
    for (const auto& [domain, mappings] : provenance.source_indices) {
        new_domain_counts.emplace(domain, mappings.size());
    }
    for (const auto& [domain, ignored] : source.domain_counts()) {
        static_cast<void>(ignored);
        if (!new_domain_counts.contains(domain)) {
            return core::Result<AttributeSet>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "topology provenance is missing a source domain"));
        }
    }
    AttributeSet result(std::move(new_domain_counts));
    for (const auto& [id, layer] : source.layers()) {
        const TransferMode mode = policy.mode_for(id);
        const auto mappings = provenance.source_indices.find(layer.descriptor.domain);
        if (mappings == provenance.source_indices.end()) {
            return core::Result<AttributeSet>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "topology provenance is missing a layer domain"));
        }
        std::vector<AttributeValue> values;
        values.reserve(mappings->second.size());
        for (const auto& sources : mappings->second) {
            auto value = transfer_value(layer, sources, mode);
            if (!value) return core::Result<AttributeSet>::failure(value.error());
            values.push_back(std::move(value.value()));
        }
        auto added = result.add_layer(AttributeLayer{layer.descriptor, std::move(values)});
        if (!added) return core::Result<AttributeSet>::failure(added.error());
    }
    return core::Result<AttributeSet>::success(std::move(result));
}

} // namespace carto::attributes
