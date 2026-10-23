#include <carto/biological/field_visualization.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace carto::biological {
namespace {

constexpr std::array<std::uint8_t, 8U> kMagic{'C', 'B', 'F', '3', '2', 'L', 'E', '1'};
constexpr std::size_t kHeaderBytes = 16U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

std::uint64_t read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::string canonical_visualization(const DerivedFieldVisualization& value) {
    std::string output;
    output.reserve(128U + value.rgba.size());
    output.append("CARTOGRAPHER_DERIVED_FIELD_PREVIEW 1\n");
    output.append(value.source.source_namespace);
    output.push_back('\n');
    output.append(std::to_string(value.source.asset_id));
    output.push_back(' ');
    output.append(std::to_string(value.source.revision.value()));
    output.push_back(' ');
    output.append(value.source.digest.hex());
    output.push_back('\n');
    output.append(value.authoring_digest.hex());
    output.push_back(' ');
    output.append(std::to_string(value.field.value));
    output.push_back(' ');
    output.append(value.sample_blob_digest.hex());
    output.push_back('\n');
    output.append(std::to_string(value.width));
    output.push_back(' ');
    output.append(std::to_string(value.height));
    output.push_back('\n');
    output.append(DerivedFieldVisualization::kLabel);
    output.push_back('\n');
    output.append(reinterpret_cast<const char*>(value.rgba.data()), value.rgba.size());
    return output;
}

bool same_visualization(const DerivedFieldVisualization& left,
    const DerivedFieldVisualization& right) {
    return left.source == right.source && left.authoring_digest == right.authoring_digest &&
        left.field == right.field && left.sample_blob_digest == right.sample_blob_digest &&
        left.width == right.width && left.height == right.height &&
        left.label == right.label && left.rgba == right.rgba &&
        left.visualization_digest == right.visualization_digest;
}

core::Result<DerivedFieldVisualization> compile(
    const BiologicalAuthoringDocument& document, FieldId field_id,
    std::span<const std::uint8_t> sample_blob, std::size_t pixel_budget) {
    if (auto result = document.validate(); !result) {
        return core::Result<DerivedFieldVisualization>::failure(result.error());
    }
    if (!field_id) {
        return core::Result<DerivedFieldVisualization>::failure(invalid(
            "biological field preview requires a nonzero field identity"));
    }
    if (pixel_budget == 0U || pixel_budget > DerivedFieldVisualization::kMaxSamples) {
        return core::Result<DerivedFieldVisualization>::failure(invalid(
            "biological field preview pixel budget is outside its hard bound"));
    }
    const auto found = document.fields().find(field_id);
    if (found == document.fields().end()) {
        return core::Result<DerivedFieldVisualization>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "biological field preview references a missing field"));
    }
    const auto& field = found->second;
    if (field.value_kind != FieldValueKind::scalar ||
        field.representation != FieldRepresentation::statistical_shading) {
        return core::Result<DerivedFieldVisualization>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                "preview supports scalar statistical-shading fields only"));
    }
    if (field.sample_count > DerivedFieldVisualization::kMaxSamples ||
        field.sample_count > pixel_budget) {
        return core::Result<DerivedFieldVisualization>::failure(invalid(
            "biological field preview exceeds the caller or hard pixel budget"));
    }
    const std::uint64_t expected_size = static_cast<std::uint64_t>(kHeaderBytes) +
        field.sample_count * sizeof(std::uint32_t);
    if (sample_blob.size() != expected_size || sample_blob.size() < kHeaderBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), sample_blob.begin()) ||
        read_u64_le(sample_blob, 8U) != field.sample_count) {
        return core::Result<DerivedFieldVisualization>::failure(invalid(
            "biological field sample blob has an unsupported header or length"));
    }
    const auto blob_digest = assets::sha256(sample_blob);
    if (blob_digest != field.data_digest) {
        return core::Result<DerivedFieldVisualization>::failure(stale(
            "biological field sample blob digest differs from authored data"));
    }

    DerivedFieldVisualization result{
        .source = document.source,
        .authoring_digest = {},
        .field = field_id,
        .sample_blob_digest = blob_digest,
        .width = static_cast<std::uint32_t>(field.sample_count),
        .height = 1U,
        .label = std::string(DerivedFieldVisualization::kLabel),
        .rgba = {},
        .visualization_digest = {},
    };
    const auto authoring_digest = document.canonical_digest();
    if (!authoring_digest) {
        return core::Result<DerivedFieldVisualization>::failure(authoring_digest.error());
    }
    result.authoring_digest = authoring_digest.value();
    result.rgba.reserve(static_cast<std::size_t>(field.sample_count) * 4U);
    for (std::size_t index = 0U; index < static_cast<std::size_t>(field.sample_count); ++index) {
        const std::size_t offset = kHeaderBytes + index * sizeof(std::uint32_t);
        const float decoded = std::bit_cast<float>(read_u32_le(sample_blob, offset));
        const double value = static_cast<double>(decoded);
        if (!std::isfinite(value) || value < field.minimum || value > field.maximum) {
            return core::Result<DerivedFieldVisualization>::failure(invalid(
                "biological field sample is non-finite or outside authored value bounds"));
        }
        const double normalized = field.maximum == field.minimum ? 0.5 :
            (value - field.minimum) / (field.maximum - field.minimum);
        const auto gray = static_cast<std::uint8_t>(std::clamp(
            std::floor(normalized * 255.0 + 0.5), 0.0, 255.0));
        result.rgba.insert(result.rgba.end(), {gray, gray, gray, 255U});
    }
    const std::string canonical = canonical_visualization(result);
    result.visualization_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    return core::Result<DerivedFieldVisualization>::success(std::move(result));
}

} // namespace

core::Result<DerivedFieldVisualization> render_scalar_field_preview(
    const BiologicalAuthoringDocument& document, FieldId field,
    std::span<const std::uint8_t> sample_blob, std::size_t pixel_budget) {
    static_assert(std::numeric_limits<float>::is_iec559,
        "biological scalar preview requires IEEE-754 float32");
    return compile(document, field, sample_blob, pixel_budget);
}

core::Result<void> DerivedFieldVisualization::validate_against(
    const BiologicalAuthoringDocument& document,
    std::span<const std::uint8_t> sample_blob) const {
    if (source != document.source) {
        return core::Result<void>::failure(stale(
            "biological field visualization source identity or revision is stale"));
    }
    const auto expected = compile(document, field, sample_blob, kMaxSamples);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (!same_visualization(*this, expected.value())) {
        return core::Result<void>::failure(stale(
            "biological field visualization differs from current authored data"));
    }
    return core::Result<void>::success();
}

} // namespace carto::biological
